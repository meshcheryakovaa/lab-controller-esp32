#include "platform/esp32/YandexDiskClient.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <cstring>

#include "core/Format.h"
#include "platform/esp32/YandexCa.h"
#include "services/CloudPath.h"

namespace lc {
namespace platform {
namespace {

/** Percent-encodes a remote path for a query parameter.  The path characters
 *  themselves are already restricted by CloudPath, but ':' and '/' still have
 *  to survive as data rather than as URL structure. */
bool encodePath(const char* input, char* out, std::size_t capacity) {
  static const char kHex[] = "0123456789ABCDEF";
  std::size_t used = 0;
  for (const char* p = input; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~';
    if (unreserved) {
      if (used + 2 > capacity) return false;
      out[used++] = static_cast<char>(c);
    } else {
      if (used + 4 > capacity) return false;
      out[used++] = '%';
      out[used++] = kHex[(c >> 4) & 0x0F];
      out[used++] = kHex[c & 0x0F];
    }
  }
  if (used + 1 > capacity) return false;
  out[used] = '\0';
  return true;
}

}  // namespace

CloudResult YandexDiskClient::refreshAuthorizationIfNeeded() {
  return oauth_.refreshIfNeeded();
}

/**
 * Turns an HTTP answer into a decision.
 *
 * The classification is the point: the state machine above needs "retry",
 * "give up" or "ask a person", and a status code is the wrong vocabulary for
 * that.  Getting this table wrong is how a device either hammers a service that
 * asked it to stop, or gives up on a blip.
 */
CloudResult YandexDiskClient::classify(int httpStatus, const JsonDocument& body,
                                       std::uint32_t retryAfterMs) {
  const char* message = body["message"] | body["description"] | "";

  if (httpStatus <= 0) {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                     "the cloud did not answer");
  }
  if (httpStatus == 401) {
    return cloudFail(CloudFailure::kUnauthorized, ErrorCode::kCloudUnauthorized,
                     "the access token was refused");
  }
  if (httpStatus == 403) {
    // Yandex uses 403 both for "no permission" and for "no space".
    const char* error = body["error"] | "";
    if (std::strstr(error, "Space") != nullptr ||
        std::strstr(message, "space") != nullptr) {
      return cloudFail(CloudFailure::kQuotaExceeded,
                       ErrorCode::kCloudQuotaExceeded,
                       "there is no free space in the cloud account");
    }
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kForbidden,
                     "the account did not allow that");
  }
  if (httpStatus == 429) {
    CloudResult result =
        cloudFail(CloudFailure::kRateLimited, ErrorCode::kRateLimited,
                  "the cloud asked us to slow down");
    result.retryAfterMs = retryAfterMs;
    return result;
  }
  if (httpStatus == 507) {
    return cloudFail(CloudFailure::kQuotaExceeded, ErrorCode::kCloudQuotaExceeded,
                     "there is no free space in the cloud account");
  }
  if (httpStatus >= 500) {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                     "the cloud reported a server error");
  }
  return cloudFail(CloudFailure::kPermanent, ErrorCode::kInternal,
                   message[0] != '\0' ? message : "the request was refused");
}

CloudResult YandexDiskClient::request(const char* method, const char* url,
                                      JsonDocument* out, int& httpStatus) {
  httpStatus = 0;
  if (!cloudCertificateConfigured()) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kCloudNotConfigured,
                     "no root certificate is built in; see YandexCa.h");
  }
  if (clock_.epochMillis() < 1600000000000ull) {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                     "the clock is not set yet");
  }

  WiFiClientSecure client;
  client.setCACert(kYandexRootCa);
  client.setTimeout(kRequestTimeoutMs / 1000);

  HTTPClient http;
  if (!http.begin(client, url)) {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                     "could not reach the cloud");
  }
  http.setTimeout(kRequestTimeoutMs);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  char header[kOAuthTokenLength + 16];
  std::size_t used = 0;
  appendFormat(header, sizeof(header), used, "OAuth %s", oauth_.accessToken());
  // The token goes ONLY to cloud-api.yandex.net.  The upload PUT below is on a
  // different host and deliberately carries none.
  http.addHeader("Authorization", header);
  http.addHeader("Accept", "application/json");
  const char* retryAfter = "Retry-After";
  http.collectHeaders(&retryAfter, 1);

  const int status = http.sendRequest(method);
  httpStatus = status;
  std::uint32_t retryAfterMs = 0;
  if (status == 429) {
    retryAfterMs = http.header("Retry-After").toInt() * 1000u;
  }

  JsonDocument body;
  if (status > 0) {
    if (http.getSize() > static_cast<int>(kMaxResponseBytes)) {
      http.end();
      return cloudFail(CloudFailure::kTransient, ErrorCode::kPayloadTooLarge,
                       "the cloud answer is unexpectedly large");
    }
    const String payload = http.getString();
    if (payload.length() <= kMaxResponseBytes && payload.length() > 0) {
      deserializeJson(body, payload);
    }
  }
  http.end();

  if (status >= 200 && status < 300) {
    if (out != nullptr) *out = body;
    return cloudOk();
  }
  return classify(status, body, retryAfterMs);
}

CloudResult YandexDiskClient::ensureDirectory(const char* path) {
  // Every missing parent, shallowest first.  Yandex creates one level at a
  // time, and a 409 for one that already exists is success.
  char partial[kCloudPathLength];
  std::size_t length = 0;
  const char* cursor = path;

  // Skip "disk:/" — it is not a folder anybody creates.
  const char* start = std::strchr(path, '/');
  if (start == nullptr) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kInvalidArgument,
                     "the remote path is not usable");
  }
  length = static_cast<std::size_t>(start - path) + 1;
  if (length >= sizeof(partial)) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kPayloadTooLarge,
                     "the remote path is too long");
  }
  std::memcpy(partial, path, length);
  partial[length] = '\0';
  cursor = start + 1;

  while (*cursor != '\0') {
    const char* slash = std::strchr(cursor, '/');
    const std::size_t part =
        slash != nullptr ? static_cast<std::size_t>(slash - cursor)
                         : std::strlen(cursor);
    if (length + part + 1 >= sizeof(partial)) {
      return cloudFail(CloudFailure::kPermanent, ErrorCode::kPayloadTooLarge,
                       "the remote path is too long");
    }
    std::memcpy(partial + length, cursor, part);
    length += part;
    partial[length] = '\0';

    char encoded[kCloudPathLength * 3];
    if (!encodePath(partial, encoded, sizeof(encoded))) {
      return cloudFail(CloudFailure::kPermanent, ErrorCode::kPayloadTooLarge,
                       "the remote path is too long");
    }
    char url[kCloudPathLength * 3 + 96];
    std::size_t used = 0;
    appendFormat(url, sizeof(url), used, "%s/resources?path=%s", kApiBase,
                 encoded);

    int status = 0;
    const CloudResult made = request("PUT", url, nullptr, status);
    if (!made.ok() && status != 409) return made;
    // 409 means it is already there.  It is confirmed to be a DIRECTORY below
    // rather than assumed: a file at that path would fail every later step in a
    // much more confusing way.
    if (status == 409) {
      CloudObjectInfo info;
      const CloudResult probed = stat(partial, info);
      if (!probed.ok()) return probed;
      if (info.exists && info.isFile) {
        return cloudFail(CloudFailure::kPermanent, ErrorCode::kCloudRemoteConflict,
                         "a file already occupies that folder name");
      }
    }

    if (slash == nullptr) break;
    partial[length++] = '/';
    partial[length] = '\0';
    cursor = slash + 1;
  }
  return cloudOk();
}

CloudResult YandexDiskClient::stat(const char* path, CloudObjectInfo& out) {
  out = CloudObjectInfo{};
  char encoded[kCloudPathLength * 3];
  if (!encodePath(path, encoded, sizeof(encoded))) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kPayloadTooLarge,
                     "the remote path is too long");
  }
  char url[kCloudPathLength * 3 + 128];
  std::size_t used = 0;
  // Only the three fields that decide anything.  Asking for the whole resource
  // would bring a listing back with it.
  appendFormat(url, sizeof(url), used,
               "%s/resources?path=%s&fields=type,size,md5", kApiBase, encoded);

  JsonDocument body;
  int status = 0;
  const CloudResult answered = request("GET", url, &body, status);
  if (status == 404) {
    // Not an error: "is it already there?" is a question the idempotency rules
    // ask deliberately, and "no" is a useful answer.
    return cloudOk();
  }
  if (!answered.ok()) return answered;

  out.exists = true;
  out.isFile = std::strcmp(body["type"] | "", "file") == 0;
  out.size = body["size"] | 0ull;
  out.md5.assign(body["md5"] | "");
  return cloudOk();
}

CloudResult YandexDiskClient::requestUploadUrl(const char* remotePath,
                                               FixedString<256>& href) {
  char encoded[kCloudPathLength * 3];
  if (!encodePath(remotePath, encoded, sizeof(encoded))) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kPayloadTooLarge,
                     "the remote path is too long");
  }
  char url[kCloudPathLength * 3 + 128];
  std::size_t used = 0;
  // overwrite=false, always.  §9 forbids overwrite for log files: a name
  // collision must stop and ask, never destroy whatever was there.
  appendFormat(url, sizeof(url), used,
               "%s/resources/upload?overwrite=false&path=%s", kApiBase, encoded);

  JsonDocument body;
  int status = 0;
  const CloudResult answered = request("GET", url, &body, status);
  if (!answered.ok()) return answered;

  const char* target = body["href"] | "";
  if (target[0] == '\0') {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kInternal,
                     "the cloud returned no upload address");
  }
  // The ONE destination in this feature chosen by a remote answer.  Checked
  // before a byte moves; an unknown host stops the job rather than sending
  // somebody's measurements to it.
  if (!isTrustedUploadUrl(target)) {
    return cloudFail(CloudFailure::kUntrustedHost, ErrorCode::kCloudUntrustedHost,
                     "the cloud offered an upload address we do not trust");
  }
  if (!href.assign(target)) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kPayloadTooLarge,
                     "the upload address is too long");
  }
  return cloudOk();
}

CloudResult YandexDiskClient::upload(const char* remotePath,
                                     IStorageBackend& storage,
                                     const char* localPath, std::uint64_t bytes,
                                     ICloudUploadObserver* observer) {
  FixedString<256> href;
  const CloudResult target = requestUploadUrl(remotePath, href);
  if (!target.ok()) return target;

  WiFiClientSecure client;
  client.setCACert(kYandexRootCa);
  client.setTimeout(kRequestTimeoutMs / 1000);

  HTTPClient http;
  if (!http.begin(client, href.c_str())) {
    return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                     "could not reach the upload address");
  }
  http.setTimeout(kRequestTimeoutMs);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  // NO Authorization header.  The one-time URL is itself the capability, and
  // attaching the OAuth token here would hand it to whatever host answered.
  http.addHeader("Content-Type", "text/csv");

  // A stream that pulls from the filesystem 2 KiB at a time.  HTTPClient wants
  // a Stream, and this is the adapter that gives it one without the file ever
  // being in RAM whole.
  class SegmentStream final : public Stream {
   public:
    SegmentStream(IStorageBackend& storage, const char* path, std::uint64_t total,
                  char* buffer, std::size_t capacity,
                  ICloudUploadObserver* observer)
        : storage_(storage), path_(path), total_(total), buffer_(buffer),
          capacity_(capacity), observer_(observer) {}

    int available() override {
      return static_cast<int>(total_ - sent_ + (filled_ - consumed_));
    }
    int read() override {
      if (!fill()) return -1;
      ++sent_;
      return static_cast<unsigned char>(buffer_[consumed_++]);
    }
    int peek() override {
      if (!fill()) return -1;
      return static_cast<unsigned char>(buffer_[consumed_]);
    }
    std::size_t readBytes(char* into, std::size_t length) override {
      std::size_t written = 0;
      while (written < length) {
        if (!fill()) break;
        const std::size_t take =
            (filled_ - consumed_) < (length - written) ? (filled_ - consumed_)
                                                       : (length - written);
        std::memcpy(into + written, buffer_ + consumed_, take);
        consumed_ += take;
        written += take;
        sent_ += take;
      }
      if (observer_ != nullptr) observer_->onUploadProgress(sent_, total_);
      return written;
    }
    void flush() override {}
    std::size_t write(std::uint8_t) override { return 0; }

   private:
    bool fill() {
      if (consumed_ < filled_) return true;
      if (offset_ >= total_) return false;
      const std::size_t want = static_cast<std::size_t>(
          (total_ - offset_) < capacity_ ? (total_ - offset_) : capacity_);
      const Result<std::size_t> read =
          storage_.readAt(path_, static_cast<std::size_t>(offset_), buffer_, want);
      if (!read.ok() || read.value() == 0) return false;
      offset_ += read.value();
      filled_ = read.value();
      consumed_ = 0;
      return true;
    }

    IStorageBackend& storage_;
    const char* path_;
    std::uint64_t total_;
    char* buffer_;
    std::size_t capacity_;
    ICloudUploadObserver* observer_;
    std::uint64_t offset_ = 0;
    std::uint64_t sent_ = 0;
    std::size_t filled_ = 0;
    std::size_t consumed_ = 0;
  };

  SegmentStream stream(storage, localPath, bytes, buffer_, sizeof(buffer_),
                       observer);
  const int status = http.sendRequest("PUT", &stream, bytes);
  http.end();

  if (status >= 200 && status < 300) return cloudOk();
  JsonDocument empty;
  return classify(status, empty, 0);
}

CloudResult YandexDiskClient::move(const char* from, const char* to) {
  char encodedFrom[kCloudPathLength * 3];
  char encodedTo[kCloudPathLength * 3];
  if (!encodePath(from, encodedFrom, sizeof(encodedFrom)) ||
      !encodePath(to, encodedTo, sizeof(encodedTo))) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kPayloadTooLarge,
                     "the remote path is too long");
  }
  char url[kCloudPathLength * 6 + 128];
  std::size_t used = 0;
  appendFormat(url, sizeof(url), used,
               "%s/resources/move?from=%s&path=%s&overwrite=false", kApiBase,
               encodedFrom, encodedTo);

  JsonDocument body;
  int status = 0;
  const CloudResult moved = request("POST", url, &body, status);
  if (!moved.ok()) return moved;

  // 202 means the move is asynchronous.  The caller verifies the final object
  // afterwards anyway, so a not-yet-finished move simply shows up as "does not
  // match yet" and is retried — no operation polling needed to be correct.
  return cloudOk();
}

CloudResult YandexDiskClient::remove(const char* path) {
  char encoded[kCloudPathLength * 3];
  if (!encodePath(path, encoded, sizeof(encoded))) {
    return cloudFail(CloudFailure::kPermanent, ErrorCode::kPayloadTooLarge,
                     "the remote path is too long");
  }
  char url[kCloudPathLength * 3 + 128];
  std::size_t used = 0;
  appendFormat(url, sizeof(url), used,
               "%s/resources?path=%s&permanently=true", kApiBase, encoded);
  int status = 0;
  const CloudResult removed = request("DELETE", url, nullptr, status);
  if (status == 404) return cloudOk();  // already gone
  return removed;
}

CloudResult YandexDiskClient::checkAccess(std::uint64_t& totalBytes,
                                          std::uint64_t& usedBytes) {
  char url[128];
  std::size_t used = 0;
  appendFormat(url, sizeof(url), used, "%s?fields=total_space,used_space",
               kApiBase);
  JsonDocument body;
  int status = 0;
  const CloudResult answered = request("GET", url, &body, status);
  if (!answered.ok()) return answered;
  totalBytes = body["total_space"] | 0ull;
  usedBytes = body["used_space"] | 0ull;
  return cloudOk();
}

}  // namespace platform
}  // namespace lc
