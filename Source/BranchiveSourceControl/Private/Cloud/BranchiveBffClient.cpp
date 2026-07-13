// Copyright Branchive.
#include "BranchiveBffClient.h"

#include "BranchiveSourceControlLog.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"

namespace
{
	// BUG1 — the best-effort Cloud-auth calls must never block a source-control op
	// for the old 30s. `/auth/lore-token` (the Lore-JWT mint) and `/auth/me` (the
	// identity check) both get a SHORT budget: a slow/unavailable BFF then falls
	// through to the ambient CLI auth in single-digit seconds instead of 30. The
	// interactive `/auth/token` code exchange keeps its longer budget (the user is
	// actively waiting on the sign-in window there).
	constexpr float kAuthShortTimeoutSeconds = 8.0f;

	// A completed HTTP response, or a failure.
	struct FSyncResponse
	{
		bool bConnected = false;
		int32 Code = 0;
		FString Body;
	};

	// Synchronously drive one request. MUST run off the game thread (the sign-in flow
	// already does). The game thread ticks the HTTP manager, so we simply POLL the
	// request's status here — reading the response directly from the request after it
	// finishes. This deliberately avoids an OnProcessRequestComplete lambda that would
	// capture stack state, since a late completion after a timeout/cancel could then
	// write through a dangling reference.
	FSyncResponse SendBlocking(const FString& Verb, const FString& Url, const FString& UserAgent,
		const FString& Bearer, const FString& JsonBody, float TimeoutSeconds)
	{
		FSyncResponse Out;

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(Verb);
		Request->SetTimeout(TimeoutSeconds);
		Request->SetHeader(TEXT("User-Agent"), UserAgent);
		Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
		if (!JsonBody.IsEmpty())
		{
			// Content-Type application/json forces the BFF's non-browser CSRF path (§3.3).
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetContentAsString(JsonBody);
		}
		if (!Bearer.IsEmpty())
		{
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Bearer));
		}

		Request->ProcessRequest();

		const double Deadline = FPlatformTime::Seconds() + double(TimeoutSeconds) + 5.0;
		while (!EHttpRequestStatus::IsFinished(Request->GetStatus()))
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				Request->CancelRequest();
				break;
			}
			FPlatformProcess::Sleep(0.01f);
		}

		const FHttpResponsePtr Response = Request->GetResponse();
		if (Response.IsValid() && Request->GetStatus() == EHttpRequestStatus::Succeeded)
		{
			Out.bConnected = true;
			Out.Code = Response->GetResponseCode();
			Out.Body = Response->GetContentAsString();
		}
		return Out;
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Body)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
		{
			return Obj;
		}
		return nullptr;
	}

	// Truncated, secret-free error text: bodies from these endpoints never echo the
	// request (which is where any token lives), so the response body cannot leak one.
	FString HttpError(const FString& Path, const FSyncResponse& R)
	{
		if (!R.bConnected)
		{
			return FString::Printf(TEXT("Could not reach the Branchive server (%s)."), *Path);
		}
		return FString::Printf(TEXT("Branchive %s returned HTTP %d: %s"),
			*Path, R.Code, *R.Body.Left(200));
	}
}

FBranchiveBffClient::FBranchiveBffClient(const FString& InBaseUrl)
{
	BaseUrl = InBaseUrl;
	while (BaseUrl.EndsWith(TEXT("/")))
	{
		BaseUrl.LeftChopInline(1);
	}
}

FString FBranchiveBffClient::DefaultUserAgent()
{
	// PlatformName() is ANSI (const char*) — convert to an FString first, then format
	// (%s wants a TCHAR*; the checked-format-string macro rejects a raw char/char*).
	const FString Platform = ANSI_TO_TCHAR(FPlatformProperties::PlatformName());
	return FString::Printf(TEXT("Branchive-UE5/0.3.3 (%s)"), *Platform);
}

FTokenExchangeResult FBranchiveBffClient::ExchangeCode(const FString& Code, const FString& CodeVerifier) const
{
	FTokenExchangeResult Result;

	// Build the body with the JSON writer so the code/verifier are correctly escaped
	// and never string-formatted into a log.
	FString Body;
	{
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Body);
		W->WriteObjectStart();
		W->WriteValue(TEXT("code"), Code);
		W->WriteValue(TEXT("code_verifier"), CodeVerifier);
		W->WriteObjectEnd();
		W->Close();
	}

	const FSyncResponse R = SendBlocking(TEXT("POST"), BaseUrl + TEXT("/auth/token"),
		DefaultUserAgent(), FString(), Body, 30.0f);
	if (R.Code != 200)
	{
		Result.Error = HttpError(TEXT("/auth/token"), R);
		return Result;
	}
	const TSharedPtr<FJsonObject> Obj = ParseObject(R.Body);
	if (!Obj.IsValid())
	{
		Result.Error = TEXT("Token exchange returned a malformed response.");
		return Result;
	}
	FString Bearer;
	if (!Obj->TryGetStringField(TEXT("branchive_token"), Bearer) || Bearer.IsEmpty())
	{
		Result.Error = TEXT("The Branchive server did not return a token.");
		return Result;
	}
	Result.Bearer = Bearer;
	double Expires = 0.0;
	if (Obj->TryGetNumberField(TEXT("expires_in"), Expires))
	{
		Result.ExpiresInSeconds = (int64)Expires;
	}
	Result.bSuccess = true;
	return Result;
}

FMeResult FBranchiveBffClient::Me(const FString& Bearer) const
{
	FMeResult Result;
	const FSyncResponse R = SendBlocking(TEXT("GET"), BaseUrl + TEXT("/auth/me"),
		DefaultUserAgent(), Bearer, FString(), kAuthShortTimeoutSeconds);
	if (R.Code == 401)
	{
		Result.bUnauthorized = true;
		Result.Error = TEXT("Branchive session expired or revoked.");
		return Result;
	}
	if (R.Code != 200)
	{
		Result.Error = HttpError(TEXT("/auth/me"), R);
		return Result;
	}
	const TSharedPtr<FJsonObject> Obj = ParseObject(R.Body);
	const TSharedPtr<FJsonObject>* IdObj = nullptr;
	if (!Obj.IsValid() || !Obj->TryGetObjectField(TEXT("identity"), IdObj) || !IdObj)
	{
		Result.bUnauthorized = true;
		Result.Error = TEXT("Not signed in to Branchive Cloud.");
		return Result;
	}
	FString Name, Email, Handle;
	(*IdObj)->TryGetStringField(TEXT("name"), Name);
	(*IdObj)->TryGetStringField(TEXT("email"), Email);
	(*IdObj)->TryGetStringField(TEXT("handle"), Handle);
	Result.Identity.Email = Email;
	Result.Identity.Handle = Handle;
	Result.Identity.Name = Name.IsEmpty() ? (Email.IsEmpty() ? TEXT("Branchive User") : Email) : Name;
	Result.bSuccess = true;
	return Result;
}

FLoreTokenResult FBranchiveBffClient::LoreToken(const FString& Bearer, const FString& RemoteUrl) const
{
	FLoreTokenResult Result;

	FString Body;
	{
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Body);
		W->WriteObjectStart();
		if (!RemoteUrl.IsEmpty())
		{
			W->WriteValue(TEXT("remoteUrl"), RemoteUrl);
		}
		W->WriteObjectEnd();
		W->Close();
	}

	const FSyncResponse R = SendBlocking(TEXT("POST"), BaseUrl + TEXT("/auth/lore-token"),
		DefaultUserAgent(), Bearer, Body, kAuthShortTimeoutSeconds);
	if (R.Code == 401)
	{
		Result.bUnauthorized = true;
		Result.Error = TEXT("Branchive session expired or revoked.");
		return Result;
	}
	if (R.Code != 200)
	{
		Result.Error = HttpError(TEXT("/auth/lore-token"), R);
		return Result;
	}
	const TSharedPtr<FJsonObject> Obj = ParseObject(R.Body);
	if (!Obj.IsValid())
	{
		Result.Error = TEXT("/auth/lore-token returned a malformed response.");
		return Result;
	}
	FString Token;
	if (!Obj->TryGetStringField(TEXT("token"), Token) || Token.IsEmpty())
	{
		Result.Error = TEXT("The Branchive server did not return a lore token.");
		return Result;
	}
	Result.Token = Token;
	Obj->TryGetStringField(TEXT("authUrl"), Result.AuthUrl);
	double Expires = 0.0;
	if (Obj->TryGetNumberField(TEXT("expiresIn"), Expires))
	{
		Result.ExpiresInSeconds = (int64)Expires;
	}
	Result.bSuccess = true;
	return Result;
}

void FBranchiveBffClient::Logout(const FString& Bearer) const
{
	if (Bearer.IsEmpty())
	{
		return;
	}
	SendBlocking(TEXT("POST"), BaseUrl + TEXT("/auth/logout"),
		DefaultUserAgent(), Bearer, TEXT("{}"), 15.0f); // best-effort; ignore the result
}
