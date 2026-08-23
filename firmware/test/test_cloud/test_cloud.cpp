// =============================================================================
//  Milestone 17 — the cloud uploader, without a cloud.
//
//  Everything that can destroy data in this feature is a DECISION, not a
//  protocol: when a local CSV may be deleted, what to do when the remote object
//  already exists, whether an unknown upload host is acceptable, what a dropped
//  connection means.  All of those are tested here against a fake provider,
//  because a rule that can only be checked by standing next to a board with an
//  internet connection is a rule nobody checks.
//
//  The failure this file exists to prevent is silent by nature: a segment
//  deleted locally that never actually reached the cloud leaves no trace on
//  either side.
// =============================================================================
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/Clock.h"
#include "core/EventBus.h"
#include "core/Md5.h"
#include "services/CloudManager.h"
#include "services/CloudPath.h"
#include "storage/CloudUploadQueue.h"
#include "storage/PosixBackend.h"

using namespace lc;

void setUp() {}
void tearDown() {}

namespace {

std::string makeTempRoot() {
  char pattern[] = "/tmp/lc-cloud-XXXXXX";
  const char* dir = mkdtemp(pattern);
  return dir != nullptr ? std::string(dir) : std::string("/tmp/lc-cloud-fallback");
}

std::string md5Of(const std::string& text) {
  char hex[Md5::kTextBytes];
  Md5::hashHex(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(),
               hex);
  return hex;
}

/**
 * A cloud that never leaves the process.
 *
 * It models the things the real one does that MATTER to the state machine: an
 * object store keyed by path, an upload that can be told to fail part-way, and
 * a stat() that answers honestly about what is actually stored.  What it does
 * not model is HTTP — because none of the rules being tested are about HTTP.
 */
class FakeCloud final : public ICloudProvider {
 public:
  struct Object { bool isFile = true; std::uint64_t size = 0; std::string md5; };

  std::map<std::string, Object> objects;
  std::vector<std::string> directories;
  std::vector<std::string> uploads;   // remote paths, in order
  std::vector<std::string> moves;
  std::vector<std::string> removed;

  bool authorised = true;
  CloudFailure failNext = CloudFailure::kNone;
  int failUploadsRemaining = 0;
  /** When set, upload() stores a TRUNCATED object — the shape of a connection
   *  that dropped after some bytes had arrived. */
  bool truncateUpload = false;
  int refreshCalls = 0;

  const char* name() const override { return "fake"; }
  bool authorized() const override { return authorised; }

  CloudResult refreshAuthorizationIfNeeded() override {
    ++refreshCalls;
    if (failNext == CloudFailure::kAuthRevoked) {
      const CloudFailure f = failNext;
      failNext = CloudFailure::kNone;
      authorised = false;
      return cloudFail(f, ErrorCode::kCloudAuthRevoked, "the token was revoked");
    }
    return cloudOk();
  }

  CloudResult ensureDirectory(const char* path) override {
    if (take(CloudFailure::kTransient)) {
      return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                       "temporary");
    }
    directories.push_back(path);
    return cloudOk();
  }

  CloudResult stat(const char* path, CloudObjectInfo& out) override {
    if (take(CloudFailure::kTransient)) {
      return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                       "temporary");
    }
    const auto found = objects.find(path);
    if (found == objects.end()) { out = CloudObjectInfo{}; return cloudOk(); }
    out.exists = true;
    out.isFile = found->second.isFile;
    out.size = found->second.size;
    out.md5.assign(found->second.md5.c_str());
    return cloudOk();
  }

  CloudResult upload(const char* remotePath, IStorageBackend& storage,
                     const char* localPath, std::uint64_t bytes,
                     ICloudUploadObserver* observer) override {
    if (failUploadsRemaining > 0) {
      --failUploadsRemaining;
      return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                       "the connection dropped");
    }
    if (take(CloudFailure::kQuotaExceeded)) {
      return cloudFail(CloudFailure::kQuotaExceeded,
                       ErrorCode::kCloudQuotaExceeded, "the disk is full");
    }

    // Streamed, exactly as the real one must be: 4 KiB at a time, never the
    // whole file.  The digest is computed from what was actually read.
    Md5 md5;
    char buffer[4096];
    std::size_t offset = 0;
    std::uint64_t sent = 0;
    const std::uint64_t limit = truncateUpload ? bytes / 2 : bytes;
    while (offset < limit) {
      const std::size_t want = static_cast<std::size_t>(
          (limit - offset) < sizeof(buffer) ? (limit - offset) : sizeof(buffer));
      const Result<std::size_t> read =
          storage.readAt(localPath, offset, buffer, want);
      if (!read.ok() || read.value() == 0) break;
      md5.update(reinterpret_cast<const std::uint8_t*>(buffer), read.value());
      offset += read.value();
      sent += read.value();
      if (observer != nullptr) observer->onUploadProgress(sent, bytes);
    }
    char hex[Md5::kTextBytes];
    md5.finishHex(hex);

    Object stored;
    stored.size = sent;
    stored.md5 = hex;
    objects[remotePath] = stored;
    uploads.push_back(remotePath);
    return cloudOk();
  }

  CloudResult move(const char* from, const char* to) override {
    if (take(CloudFailure::kTransient)) {
      return cloudFail(CloudFailure::kTransient, ErrorCode::kCloudTransient,
                       "temporary");
    }
    const auto found = objects.find(from);
    if (found == objects.end()) {
      return cloudFail(CloudFailure::kPermanent, ErrorCode::kNotFound, "gone");
    }
    objects[to] = found->second;
    objects.erase(found);
    moves.push_back(std::string(from) + " -> " + to);
    return cloudOk();
  }

  CloudResult remove(const char* path) override {
    objects.erase(path);
    removed.push_back(path);
    return cloudOk();
  }

 private:
  bool take(CloudFailure which) {
    if (failNext != which) return false;
    failNext = CloudFailure::kNone;
    return true;
  }
};

/** A network that is up unless a test says otherwise. */
class FakeNetwork final : public INetworkManager {
 public:
  bool up = true;
  NetworkStatus status() const override {
    NetworkStatus out;
    out.stationConnected = up;
    out.state = up ? NetworkState::kStationConnected : NetworkState::kApOnly;
    return out;
  }
  Status beginScan() override { return ok(); }
  ScanState scanState() const override { return ScanState::kIdle; }
  std::size_t scanResults(NetworkCandidate*, std::size_t) const override { return 0; }
  Status testCredentials(const char*, const char*) override { return ok(); }
  Status clearCredentials() override { return ok(); }
  Status setHostname(const char*) override { return ok(); }
};

/** Everything the uploader needs, wired the way main.cpp does. */
struct CloudRig {
  std::string root = makeTempRoot();
  ManualClock clock;
  platform::PosixBackend backend{root};
  EventBus events;
  CloudUploadQueue queue{backend};
  CloudManager manager{clock, queue, events};
  FakeCloud cloud;
  FakeNetwork network;

  CloudRig() {
    // A plausible wall clock: before this the uploader deliberately refuses to
    // start, because TLS certificates cannot be judged without one.
    clock.setEpochMillis(1787480000000ull);
    backend.ensureDirectory("/data");
    backend.ensureDirectory("/data/logs");
    manager.setProvider(&cloud);
    manager.setNetwork(&network);
    manager.setStorage(&backend);
    manager.setControllerId("esp32-a1b2c3");
    manager.setEnabled(true);
    manager.begin();
  }

  ~CloudRig() {
    const std::string command = "rm -rf " + root;
    if (std::system(command.c_str()) != 0) { /* best effort */ }
  }

  std::string writeSegment(const char* path, std::size_t bytes) {
    std::string text;
    text.reserve(bytes);
    for (std::size_t i = 0; i < bytes; ++i) text += char('a' + (i % 26));
    backend.writeAtomic(path, text.c_str(), text.size());
    return text;
  }

  /** Runs the state machine to a standstill, the way the worker task does. */
  void pump(int maxSteps = 40) {
    for (int i = 0; i < maxSteps; ++i) {
      if (!manager.tick()) return;
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
//  The rule the whole milestone exists for
// ---------------------------------------------------------------------------
static void test_a_local_file_is_deleted_only_after_the_cloud_copy_is_verified() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000001.csv", 5000);

  const Result<std::uint32_t> queued = rig.manager.enqueueSegment(
      "log_0001", "p000001", "/data/logs/log_0001_p000001.csv",
      "log_0001_p000001.csv", text.size(), "8ea4c12f");
  TEST_ASSERT_TRUE(queued.ok());
  // Still on the device: queueing proves nothing.
  TEST_ASSERT_TRUE(rig.backend.exists("/data/logs/log_0001_p000001.csv"));

  rig.pump();

  // Gone locally, and present remotely with the right bytes.
  TEST_ASSERT_FALSE(rig.backend.exists("/data/logs/log_0001_p000001.csv"));
  const std::string remote =
      "disk:/LabController/esp32-a1b2c3/log_0001/log_0001_p000001.csv";
  TEST_ASSERT_TRUE(rig.cloud.objects.count(remote) == 1);
  TEST_ASSERT_EQUAL_UINT(text.size(), rig.cloud.objects[remote].size);
  TEST_ASSERT_EQUAL_STRING(md5Of(text).c_str(), rig.cloud.objects[remote].md5.c_str());

  // It went to a TEMPORARY name first and was renamed.  That is what makes a
  // half-finished transfer distinguishable from a finished one.
  TEST_ASSERT_EQUAL_UINT(1, rig.cloud.uploads.size());
  TEST_ASSERT_TRUE(rig.cloud.uploads[0].find(".uploading") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT(1, rig.cloud.moves.size());
  TEST_ASSERT_EQUAL_UINT(0, rig.queue.size());
}

static void test_a_truncated_upload_never_becomes_an_acknowledgement() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000002.csv", 5000);
  rig.manager.enqueueSegment("log_0001", "p000002",
                             "/data/logs/log_0001_p000002.csv",
                             "log_0001_p000002.csv", text.size(), "");

  // The connection drops half way: the server stores half a file and reports
  // success.  Verification is the only thing that can tell.
  rig.cloud.truncateUpload = true;
  rig.pump();

  TEST_ASSERT_TRUE(rig.backend.exists("/data/logs/log_0001_p000002.csv"));
  const std::string remote =
      "disk:/LabController/esp32-a1b2c3/log_0001/log_0001_p000002.csv";
  TEST_ASSERT_EQUAL_UINT(0, rig.cloud.objects.count(remote));
  TEST_ASSERT_EQUAL_UINT(1, rig.queue.size());
  TEST_ASSERT_TRUE(rig.queue.at(0).state != CloudJobState::kAcknowledged);

  // And when the connection is healthy again the retry completes it.
  rig.cloud.truncateUpload = false;
  CloudJob retried = rig.queue.at(0);
  rig.manager.retry(retried.id);
  rig.pump();
  TEST_ASSERT_FALSE(rig.backend.exists("/data/logs/log_0001_p000002.csv"));
  TEST_ASSERT_EQUAL_STRING(md5Of(text).c_str(), rig.cloud.objects[remote].md5.c_str());
}

// ---------------------------------------------------------------------------
//  §9 — an unknown outcome must not create a duplicate
// ---------------------------------------------------------------------------
static void test_a_file_already_in_the_cloud_is_recognised_not_resent() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000003.csv", 3000);

  // The previous attempt actually succeeded; the answer never arrived.
  FakeCloud::Object already;
  already.size = text.size();
  already.md5 = md5Of(text);
  rig.cloud.objects["disk:/LabController/esp32-a1b2c3/log_0001/log_0001_p000003.csv"] =
      already;

  rig.manager.enqueueSegment("log_0001", "p000003",
                             "/data/logs/log_0001_p000003.csv",
                             "log_0001_p000003.csv", text.size(), "");
  rig.pump();

  // Recognised as done, the local copy released — and NOTHING was sent again.
  TEST_ASSERT_FALSE(rig.backend.exists("/data/logs/log_0001_p000003.csv"));
  TEST_ASSERT_EQUAL_UINT(0, rig.cloud.uploads.size());
  TEST_ASSERT_EQUAL_UINT(0, rig.cloud.moves.size());
}

static void test_a_different_file_at_that_path_is_never_overwritten() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000004.csv", 3000);

  // Somebody else's data, or an older run's, at the name we want.
  FakeCloud::Object other;
  other.size = 999;
  other.md5 = md5Of("something else entirely");
  const std::string remote =
      "disk:/LabController/esp32-a1b2c3/log_0001/log_0001_p000004.csv";
  rig.cloud.objects[remote] = other;

  rig.manager.enqueueSegment("log_0001", "p000004",
                             "/data/logs/log_0001_p000004.csv",
                             "log_0001_p000004.csv", text.size(), "");
  rig.pump();

  // Stopped and asked.  The remote file is untouched, ours is still here, and
  // the job says why rather than retrying into the same wall for ever.
  TEST_ASSERT_EQUAL_UINT(999, rig.cloud.objects[remote].size);
  TEST_ASSERT_TRUE(rig.backend.exists("/data/logs/log_0001_p000004.csv"));
  TEST_ASSERT_EQUAL_UINT(0, rig.cloud.uploads.size());
  TEST_ASSERT_EQUAL_UINT(1, rig.queue.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CloudJobState::kRemoteConflict),
                        static_cast<int>(rig.queue.at(0).state));
}

static void test_a_leftover_partial_upload_is_replaced_not_trusted() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000005.csv", 3000);

  // A ".uploading" from an interrupted attempt, containing the wrong bytes.
  FakeCloud::Object partial;
  partial.size = 1200;
  partial.md5 = md5Of("half of it");
  const std::string temporary =
      "disk:/LabController/esp32-a1b2c3/log_0001/log_0001_p000005.csv.uploading";
  rig.cloud.objects[temporary] = partial;

  rig.manager.enqueueSegment("log_0001", "p000005",
                             "/data/logs/log_0001_p000005.csv",
                             "log_0001_p000005.csv", text.size(), "");
  rig.pump();

  // The stale temporary was cleared and a full copy sent.  A temporary is
  // OURS by name and worthless when partial; the FINAL path never gets this
  // treatment.
  TEST_ASSERT_EQUAL_UINT(1, rig.cloud.removed.size());
  TEST_ASSERT_FALSE(rig.backend.exists("/data/logs/log_0001_p000005.csv"));
  const std::string remote =
      "disk:/LabController/esp32-a1b2c3/log_0001/log_0001_p000005.csv";
  TEST_ASSERT_EQUAL_STRING(md5Of(text).c_str(), rig.cloud.objects[remote].md5.c_str());
}

static void test_a_complete_temporary_is_moved_rather_than_re_sent() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000006.csv", 3000);

  // The bytes arrived last time; only the rename did not.
  FakeCloud::Object complete;
  complete.size = text.size();
  complete.md5 = md5Of(text);
  rig.cloud.objects[
      "disk:/LabController/esp32-a1b2c3/log_0001/log_0001_p000006.csv.uploading"] =
      complete;

  rig.manager.enqueueSegment("log_0001", "p000006",
                             "/data/logs/log_0001_p000006.csv",
                             "log_0001_p000006.csv", text.size(), "");
  rig.pump();

  // Sending 100 KiB again over a metered link because a rename failed is the
  // waste this check exists to avoid.
  TEST_ASSERT_EQUAL_UINT(0, rig.cloud.uploads.size());
  TEST_ASSERT_EQUAL_UINT(1, rig.cloud.moves.size());
  TEST_ASSERT_FALSE(rig.backend.exists("/data/logs/log_0001_p000006.csv"));
}

// ---------------------------------------------------------------------------
//  Gates: nothing is attempted when it cannot possibly work
// ---------------------------------------------------------------------------
static void test_no_network_waits_without_spending_attempts() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000007.csv", 2000);
  rig.manager.enqueueSegment("log_0001", "p000007",
                             "/data/logs/log_0001_p000007.csv",
                             "log_0001_p000007.csv", text.size(), "");

  rig.network.up = false;
  for (int i = 0; i < 20; ++i) rig.manager.tick();

  // A week in a cupboard with no router must not burn the retry schedule and
  // land every segment in PERMANENT_ERROR.
  TEST_ASSERT_EQUAL_UINT(0, rig.queue.at(0).attempts);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CloudJobState::kWaitingNetwork),
                        static_cast<int>(rig.queue.at(0).state));
  TEST_ASSERT_TRUE(rig.backend.exists("/data/logs/log_0001_p000007.csv"));
  TEST_ASSERT_EQUAL_UINT(0, rig.cloud.uploads.size());

  rig.network.up = true;
  rig.pump();
  TEST_ASSERT_FALSE(rig.backend.exists("/data/logs/log_0001_p000007.csv"));
}

static void test_an_unset_clock_stops_the_upload_rather_than_tls() {
  CloudRig rig;
  // A board that has just powered up with no SNTP yet.  Every certificate looks
  // invalid; the answer is to wait, never to stop checking certificates.
  rig.clock.setEpochMillis(1000);
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000008.csv", 2000);
  rig.manager.enqueueSegment("log_0001", "p000008",
                             "/data/logs/log_0001_p000008.csv",
                             "log_0001_p000008.csv", text.size(), "");
  for (int i = 0; i < 10; ++i) rig.manager.tick();

  TEST_ASSERT_EQUAL_INT(static_cast<int>(CloudJobState::kWaitingTime),
                        static_cast<int>(rig.queue.at(0).state));
  TEST_ASSERT_EQUAL_UINT(0, rig.cloud.uploads.size());
  TEST_ASSERT_TRUE(rig.backend.exists("/data/logs/log_0001_p000008.csv"));
}

static void test_a_revoked_token_pauses_the_queue_and_keeps_the_data() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000009.csv", 2000);
  rig.manager.enqueueSegment("log_0001", "p000009",
                             "/data/logs/log_0001_p000009.csv",
                             "log_0001_p000009.csv", text.size(), "");

  rig.cloud.failNext = CloudFailure::kAuthRevoked;
  rig.pump();

  TEST_ASSERT_EQUAL_INT(static_cast<int>(CloudJobState::kPausedNoAuth),
                        static_cast<int>(rig.queue.at(0).state));
  TEST_ASSERT_TRUE(rig.backend.exists("/data/logs/log_0001_p000009.csv"));
  TEST_ASSERT_EQUAL_UINT(0, rig.cloud.uploads.size());

  // And it stays paused rather than retrying: this needs a person.
  for (int i = 0; i < 10; ++i) rig.manager.tick();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CloudJobState::kPausedNoAuth),
                        static_cast<int>(rig.queue.at(0).state));
}

static void test_a_missing_local_file_is_an_error_not_an_acknowledgement() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000010.csv", 2000);
  rig.manager.enqueueSegment("log_0001", "p000010",
                             "/data/logs/log_0001_p000010.csv",
                             "log_0001_p000010.csv", text.size(), "");
  // Somebody deleted it by hand.
  rig.backend.remove("/data/logs/log_0001_p000010.csv");
  rig.pump();

  // The measurements are simply not here.  Recording that as "uploaded" would
  // be a lie about where the data is.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CloudJobState::kPermanentError),
                        static_cast<int>(rig.queue.at(0).state));
  TEST_ASSERT_EQUAL_UINT(0, rig.cloud.uploads.size());
}

// ---------------------------------------------------------------------------
//  The queue itself
// ---------------------------------------------------------------------------
static void test_the_queue_survives_a_reset_mid_upload() {
  const std::string root = makeTempRoot();
  {
    platform::PosixBackend backend{root};
    backend.ensureDirectory("/data");
    CloudUploadQueue queue{backend};
    TEST_ASSERT_TRUE(queue.begin().ok());

    CloudJob job;
    job.sessionId.assign("log_0001");
    job.segmentId.assign("p000001");
    job.localPath.assign("/data/logs/log_0001_p000001.csv");
    job.remotePath.assign("disk:/LabController/x/log_0001/log_0001_p000001.csv");
    job.bytes = 101824;
    job.md5.assign("4b2f1f3d6e778e338c2b75e56e2fa441");
    const Result<std::uint32_t> id = queue.enqueue(job);
    TEST_ASSERT_TRUE(id.ok());

    CloudJob mid = *queue.find(id.value());
    mid.state = CloudJobState::kUploading;
    TEST_ASSERT_TRUE(queue.update(mid).ok());
  }

  // The power went here.
  {
    platform::PosixBackend backend{root};
    CloudUploadQueue queue{backend};
    TEST_ASSERT_TRUE(queue.begin().ok());
    TEST_ASSERT_EQUAL_UINT(1, queue.size());
    // Everything survived, including the hash that will be used to decide
    // whether the interrupted transfer actually completed.
    TEST_ASSERT_EQUAL_STRING("4b2f1f3d6e778e338c2b75e56e2fa441",
                             queue.at(0).md5.c_str());
    TEST_ASSERT_EQUAL_UINT(101824, queue.at(0).bytes);

    TEST_ASSERT_EQUAL_UINT(1, queue.recoverInFlight());
    // Back to PENDING — which means "check the remote first", not "send again".
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CloudJobState::kPending),
                          static_cast<int>(queue.at(0).state));
  }
  const std::string command = "rm -rf " + root;
  if (std::system(command.c_str()) != 0) { /* best effort */ }
}

static void test_a_corrupt_queue_stops_the_uploader_and_keeps_every_file() {
  const std::string root = makeTempRoot();
  platform::PosixBackend backend{root};
  backend.ensureDirectory("/data");
  const char* rubbish = "{ this is not json";
  backend.writeAtomic(CloudUploadQueue::kPath, rubbish, std::strlen(rubbish));

  CloudUploadQueue queue{backend};
  const Status loaded = queue.begin();
  TEST_ASSERT_FALSE(loaded.ok());
  TEST_ASSERT_TRUE(queue.corrupt());
  // NOT overwritten with an empty queue.  This file is the only record of which
  // measurements exist here and nowhere else; starting fresh would turn "I
  // cannot read the schedule" into "there was nothing to send".
  TEST_ASSERT_TRUE(backend.exists(CloudUploadQueue::kPath));
  TEST_ASSERT_TRUE(queue.nextRunnable(0) == nullptr);

  const std::string command = "rm -rf " + root;
  if (std::system(command.c_str()) != 0) { /* best effort */ }
}

static void test_the_same_segment_is_never_queued_twice() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000011.csv", 1000);
  const Result<std::uint32_t> first = rig.manager.enqueueSegment(
      "log_0001", "p000011", "/data/logs/log_0001_p000011.csv",
      "log_0001_p000011.csv", text.size(), "");
  const Result<std::uint32_t> second = rig.manager.enqueueSegment(
      "log_0001", "p000011", "/data/logs/log_0001_p000011.csv",
      "log_0001_p000011.csv", text.size(), "");
  TEST_ASSERT_TRUE(first.ok() && second.ok());
  // Two jobs racing each other to one remote path is exactly the duplicate the
  // idempotency rules exist to prevent — so it is refused at the door.
  TEST_ASSERT_EQUAL_UINT(first.value(), second.value());
  TEST_ASSERT_EQUAL_UINT(1, rig.queue.size());
}

static void test_backoff_grows_and_is_spread_out() {
  // The published schedule, and a ceiling that stops it growing for ever.
  TEST_ASSERT_TRUE(CloudUploadQueue::backoffMs(1, 0) == 5000);
  TEST_ASSERT_TRUE(CloudUploadQueue::backoffMs(2, 0) == 15000);
  TEST_ASSERT_TRUE(CloudUploadQueue::backoffMs(6, 0) == 1800000);
  TEST_ASSERT_TRUE(CloudUploadQueue::backoffMs(99, 0) ==
                   CloudUploadQueue::kMaxBackoffMs);

  // Jitter never shortens a delay and never adds more than 20 %.  Several
  // controllers losing one router must not come back in lockstep.
  for (std::uint32_t j = 0; j < 5000; j += 137) {
    const std::uint32_t delay = CloudUploadQueue::backoffMs(1, j);
    TEST_ASSERT_TRUE(delay >= 5000);
    TEST_ASSERT_TRUE(delay <= 6000);
  }
}

// ---------------------------------------------------------------------------
//  Paths and upload URLs
// ---------------------------------------------------------------------------
static void test_a_remote_path_cannot_escape_its_folder() {
  CloudPathString path;

  // Traversal, in the three shapes it arrives in.
  TEST_ASSERT_TRUE(buildCloudSessionPath("disk:/LabController", "esp32",
                                         "../../etc", path));
  TEST_ASSERT_TRUE(std::strstr(path.c_str(), "..") == nullptr);

  char clean[32];
  TEST_ASSERT_FALSE(sanitiseCloudComponent("..", clean, sizeof(clean)));
  TEST_ASSERT_FALSE(sanitiseCloudComponent(".", clean, sizeof(clean)));
  TEST_ASSERT_FALSE(sanitiseCloudComponent("", clean, sizeof(clean)));
  TEST_ASSERT_FALSE(sanitiseCloudComponent("///", clean, sizeof(clean)));

  // A slash inside a component becomes a dash rather than a new folder, and
  // "a/b" therefore cannot collide with "ab".
  TEST_ASSERT_TRUE(sanitiseCloudComponent("a/b", clean, sizeof(clean)));
  TEST_ASSERT_EQUAL_STRING("a-b", clean);
  TEST_ASSERT_TRUE(sanitiseCloudComponent("run 1\ttab", clean, sizeof(clean)));
  TEST_ASSERT_EQUAL_STRING("run-1-tab", clean);

  // A root is normalised whatever the operator typed.
  CloudPathString root;
  TEST_ASSERT_TRUE(normaliseCloudRoot("Lab Data", root));
  TEST_ASSERT_EQUAL_STRING("disk:/Lab-Data", root.c_str());
  TEST_ASSERT_TRUE(normaliseCloudRoot("disk:///a//b///", root));
  TEST_ASSERT_EQUAL_STRING("disk:/a/b", root.c_str());
  TEST_ASSERT_FALSE(normaliseCloudRoot("", root));
  TEST_ASSERT_FALSE(normaliseCloudRoot("/", root));

  // And the whole path is deterministic — the same segment always lands in the
  // same place, which is what makes a retry check rather than duplicate.
  CloudPathString a;
  CloudPathString b;
  buildCloudSessionPath("disk:/LabController", "esp32-a1b2c3", "log_0001", a);
  buildCloudSessionPath("disk:/LabController", "esp32-a1b2c3", "log_0001", b);
  TEST_ASSERT_EQUAL_STRING(a.c_str(), b.c_str());
  TEST_ASSERT_EQUAL_STRING("disk:/LabController/esp32-a1b2c3/log_0001", a.c_str());
}

static void test_an_upload_url_outside_yandex_is_refused() {
  // The one place in this feature where the destination comes from a REMOTE
  // answer.  A wrong or tampered response must not be able to send a dataset
  // to somebody else's server.
  TEST_ASSERT_TRUE(isTrustedUploadUrl(
      "https://uploader1d.disk.yandex.net/upload-target/12345"));
  TEST_ASSERT_TRUE(isTrustedUploadUrl("https://storage.yandex.net/x"));

  TEST_ASSERT_FALSE(isTrustedUploadUrl("http://uploader.disk.yandex.net/x"));
  TEST_ASSERT_FALSE(isTrustedUploadUrl("https://example.com/x"));
  // The classic way a suffix check is defeated.
  TEST_ASSERT_FALSE(isTrustedUploadUrl("https://evil-disk.yandex.net.attacker.com/x"));
  TEST_ASSERT_FALSE(isTrustedUploadUrl("https://yandex.net.evil.com/x"));
  // Credentials in the authority are a shape worth refusing outright.
  TEST_ASSERT_FALSE(isTrustedUploadUrl("https://a@evil.com/x"));
  TEST_ASSERT_FALSE(isTrustedUploadUrl(""));
  TEST_ASSERT_FALSE(isTrustedUploadUrl("ftp://disk.yandex.net/x"));
}

// ---------------------------------------------------------------------------
//  What the API is allowed to say
// ---------------------------------------------------------------------------
static void test_the_status_document_carries_no_secret() {
  CloudRig rig;
  const std::string text = rig.writeSegment("/data/logs/log_0001_p000012.csv", 1000);
  rig.manager.enqueueSegment("log_0001", "p000012",
                             "/data/logs/log_0001_p000012.csv",
                             "log_0001_p000012.csv", text.size(), "");

  JsonDocument document;
  rig.manager.describe(document.to<JsonObject>());
  std::string json;
  serializeJson(document, json);

  // No token, no client secret, and no upload URL — the last of those is a
  // one-time capability that would let anyone holding it write to the account.
  for (const char* forbidden : {"token", "secret", "Authorization", "uploader",
                                "href", "client_id", "clientSecret"}) {
    TEST_ASSERT_TRUE(json.find(forbidden) == std::string::npos);
  }
  // But it does say the things an operator needs.
  TEST_ASSERT_TRUE(json.find("rootPath") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("queue") != std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_local_file_is_deleted_only_after_the_cloud_copy_is_verified);
  RUN_TEST(test_a_truncated_upload_never_becomes_an_acknowledgement);
  RUN_TEST(test_a_file_already_in_the_cloud_is_recognised_not_resent);
  RUN_TEST(test_a_different_file_at_that_path_is_never_overwritten);
  RUN_TEST(test_a_leftover_partial_upload_is_replaced_not_trusted);
  RUN_TEST(test_a_complete_temporary_is_moved_rather_than_re_sent);
  RUN_TEST(test_no_network_waits_without_spending_attempts);
  RUN_TEST(test_an_unset_clock_stops_the_upload_rather_than_tls);
  RUN_TEST(test_a_revoked_token_pauses_the_queue_and_keeps_the_data);
  RUN_TEST(test_a_missing_local_file_is_an_error_not_an_acknowledgement);
  RUN_TEST(test_the_queue_survives_a_reset_mid_upload);
  RUN_TEST(test_a_corrupt_queue_stops_the_uploader_and_keeps_every_file);
  RUN_TEST(test_the_same_segment_is_never_queued_twice);
  RUN_TEST(test_backoff_grows_and_is_spread_out);
  RUN_TEST(test_a_remote_path_cannot_escape_its_folder);
  RUN_TEST(test_an_upload_url_outside_yandex_is_refused);
  RUN_TEST(test_the_status_document_carries_no_secret);
  return UNITY_END();
}
