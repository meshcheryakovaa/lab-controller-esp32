// =============================================================================
//  services/CloudPath.h — building remote paths that cannot escape (M17).
//
//  A remote path is assembled from three things the operator can influence: the
//  root folder they typed, the controller id, and the session and segment names
//  that came out of LogStore.  Any of them could contain a "..", a slash, a
//  control character or a byte that is not ASCII, and the result would be a
//  request to write somewhere nobody intended.
//
//  So paths are BUILT here, from sanitised parts, and never concatenated at the
//  call site.  The function is platform-independent and pure precisely so the
//  awkward inputs can be thrown at it by a host test instead of by an accident.
//
//  Determinism matters as much as safety: the same segment must always produce
//  the same remote path, because that is what makes a retry after an unknown
//  outcome check the right object rather than create a second one (§9).
// =============================================================================
#pragma once

#include <cstddef>

#include "core/Error.h"
#include "core/Types.h"

namespace lc {

/** Longest remote path this builds.  Yandex allows more; this is bounded
 *  because every buffer downstream of it is. */
inline constexpr std::size_t kCloudPathLength = 160;
/** The suffix a file wears while it is being uploaded.  A partial transfer is
 *  therefore never mistakable for a finished dataset — not by this controller,
 *  and not by a person looking at the folder. */
inline constexpr const char* kUploadingSuffix = ".uploading";

using CloudPathString = FixedString<kCloudPathLength>;

/**
 * Cleans one path COMPONENT.
 *
 * Keeps `[A-Za-z0-9._-]`, turns runs of anything else into a single `-`, and
 * refuses the results that are dangerous rather than merely ugly: empty, `.`
 * and `..`.  Returns false when the component cannot be made safe, and the
 * caller must then refuse the whole operation — quietly substituting something
 * would put a file in a place the operator did not choose.
 */
bool sanitiseCloudComponent(const char* input, char* out, std::size_t capacity);

/**
 * Normalises the root the operator typed: "disk:/LabController".
 *
 * Accepts it with or without the "disk:" prefix and with any number of stray
 * slashes, and applies the component rules to every part in between.
 */
bool normaliseCloudRoot(const char* input, CloudPathString& out);

/** root / controller / session — the folder one recording lives in. */
bool buildCloudSessionPath(const char* root, const char* controllerId,
                           const char* sessionId, CloudPathString& out);

/** The final path of one segment inside that folder. */
bool buildCloudSegmentPath(const char* sessionPath, const char* fileName,
                           CloudPathString& out);

/** The same path with the ".uploading" suffix. */
bool buildCloudTemporaryPath(const char* finalPath, CloudPathString& out);

/**
 * Is this host one an upload may be sent to?
 *
 * Yandex hands back a one-time `href` on an uploader host, and that URL is the
 * one place in this feature where the destination is chosen by a REMOTE answer
 * rather than by us.  A compromised or simply wrong response must not be able
 * to redirect a dataset to an arbitrary server, so the host is checked against
 * the provider's own domains and the scheme must be https.
 *
 * Deliberately a suffix match on the domain rather than one hard-coded machine:
 * the uploader pool has many names and they change.  Pinning one observed host
 * would break the feature the first time Yandex added a server.
 */
bool isTrustedUploadUrl(const char* url);

}  // namespace lc
