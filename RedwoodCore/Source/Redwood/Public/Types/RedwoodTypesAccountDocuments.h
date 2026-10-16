// Copyright 2026 God Mode Games, LLC. All Rights Reserved.

// FORK(hollowed-oath): entire file is fork-added -- no upstream counterpart.

// Account documents: small JSON documents that belong to an account (player
// id), not to a character, stored by the realm `AccountDocument` table. They
// hold account STATE. They are
// NEVER for items or currency: those need atomic transfers that cannot dupe,
// and items belong in the character item rows.
//
// Plain C++ types: nothing here is exposed to Blueprints.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace RedwoodAccountDocumentLimits {
  // The same rules as the backend's AccountDocuments request schema. The
  // player id and the key become file names offline, so the charset must not
  // let them leave the folder.
  constexpr int32 MaxIdLength = 64;

  // The backend's AccountDocuments.MaxDepth: the deepest nesting of objects
  // and arrays, the data object being level 1. Deeper data can overflow the
  // stack of the recursive JSON code (the serializer here, socket.io).
  constexpr int32 MaxDepth = 64;

  // The backend's default `persistence.account-documents.max-bytes`: the
  // document data as compact JSON text, in UTF-8 bytes. It must stay under
  // socket.io's maxHttpBufferSize (1e6), or the socket drops a bigger message
  // instead of returning tooLarge.
  constexpr int32 MaxDataBytes = 524288;
} // namespace RedwoodAccountDocumentLimits

struct FRedwoodAccountDocument {
  // Starts at 1 on create and grows by 1 on every successful put.
  int32 Revision = 0;

  TSharedPtr<FJsonObject> Data;
};

struct FRedwoodGetAccountDocumentOutput {
  FString Error;

  // False when the account has no document with this key. Document is then
  // unset.
  bool bFound = false;

  FRedwoodAccountDocument Document;
};

typedef TDelegate<void(const FRedwoodGetAccountDocumentOutput &)>
  FRedwoodGetAccountDocumentOutputDelegate;

struct FRedwoodPutAccountDocumentOutput {
  // Set when the put could not be tried or failed. A conflict is not an error.
  // An error that comes after the request was sent (for example the sidecar's
  // relay to the realm timed out) can mean the outcome is unknown: the write
  // may have landed. Get before a retry.
  FString Error;

  // True when the document was not at the expected revision, so nothing was
  // written.
  bool bConflict = false;

  // The new revision after a successful put, otherwise 0.
  int32 Revision = 0;

  // True when the document is too large. bRejected is also true then.
  bool bTooLarge = false;

  // True when the put can never succeed as it is (too large, too deep, or
  // another refusal the backend makes final). Retrying the same data cannot
  // succeed, so callers treat it as final and branch on this, not on Error's
  // text.
  bool bRejected = false;
};

typedef TDelegate<void(const FRedwoodPutAccountDocumentOutput &)>
  FRedwoodPutAccountDocumentOutputDelegate;
