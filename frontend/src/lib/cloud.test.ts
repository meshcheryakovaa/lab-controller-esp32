// =============================================================================
//  cloud.test.ts — the Yandex Disk client, without Yandex (npm test).
//
//  M17 is the first feature where this browser handles a credential that is not
//  the operator's own device password: an OAuth client secret belonging to their
//  Yandex account.  These tests are about what happens to it, and about the one
//  thing the page must never do — imply that a segment is safely in the cloud
//  when it is still only on the controller.
// =============================================================================

import { beforeEach, describe, expect, it, vi } from 'vitest';
import { api } from './api';
import type { CloudStatus } from './types';

interface Call { method: string; url: string; body: unknown }

let calls: Call[] = [];
let reply: (call: Call) => { status: number; body: unknown };

function idle(): CloudStatus {
  return {
    provider: 'yandex',
    enabled: true,
    authorized: true,
    configured: true,
    clientSecretSet: true,
    state: 'IDLE',
    linkState: 'AUTHORIZED',
    rootPath: 'disk:/LabController',
    networkReady: true,
    timeReady: true,
    tokenExpiresAt: 1787482800000,
    lastSuccessEpochMs: 1787480012345,
    secureStorage: false,
    queue: { paused: false, corrupt: false, pending: 0, failed: 0,
             acknowledged: 14, jobs: [] },
    lastError: null,
  };
}

beforeEach(() => {
  calls = [];
  reply = () => ({ status: 200, body: idle() });
  vi.stubGlobal('location', { origin: 'http://192.168.1.74' });
  vi.stubGlobal('fetch', async (url: string, init: RequestInit = {}) => {
    const call: Call = {
      method: init.method ?? 'GET',
      url: String(url),
      body: init.body ? JSON.parse(String(init.body)) : undefined,
    };
    calls.push(call);
    const { status, body } = reply(call);
    return {
      ok: status < 400,
      status,
      json: async () => body,
      text: async () => JSON.stringify(body),
      headers: new Headers({ 'content-type': 'application/json' }),
    } as unknown as Response;
  });
});

describe('the cloud client', () => {
  it('sends the client secret once and never reads one back', async () => {
    await api.saveCloudConfig({
      clientId: 'my-client-id',
      clientSecret: 'top-secret-value',
      rootPath: 'disk:/LabController',
      enabled: true,
    });

    const put = calls.find((c) => c.method === 'PUT')!;
    expect(put.body).toMatchObject({ clientSecret: 'top-secret-value' });
    // Never in a URL, where it would reach a proxy log or browser history.
    for (const call of calls) expect(call.url).not.toContain('top-secret-value');

    // And nothing that comes back has anywhere to put one: the status type has
    // no secret field, mirroring a firmware interface with no method to return
    // one.
    const status = await api.cloud();
    expect(JSON.stringify(status)).not.toContain('top-secret-value');
    expect('clientSecret' in status).toBe(false);
    expect(status.clientSecretSet).toBe(true);
  });

  it('omits the secret entirely when it is not being changed', async () => {
    // The page cannot show the stored secret, so it must not force a re-type to
    // change something else on the same form.  Sending an empty string would be
    // read by the firmware as "leave it alone" too, but not sending the key at
    // all is what the client does.
    await api.saveCloudConfig({ rootPath: 'disk:/Other' });
    const put = calls.find((c) => c.method === 'PUT')!;
    expect(put.body).toEqual({ rootPath: 'disk:/Other' });
    expect(Object.keys(put.body as object)).not.toContain('clientSecret');
  });

  it('asks for a device code and lets the controller do the waiting', async () => {
    reply = (call) => {
      if (call.url.endsWith('/device-code')) {
        return { status: 200, body: { userCode: 'ABCD-1234',
                 verificationUrl: 'https://oauth.yandex.ru/device',
                 expiresIn: 300 } };
      }
      return { status: 200, body: { state: 'WAITING_USER', expiresIn: 241 } };
    };

    const prompt = await api.beginCloudLink();
    expect(prompt.userCode).toBe('ABCD-1234');
    expect(prompt.verificationUrl).toContain('oauth.yandex.ru');

    // The page polls the CONTROLLER, never Yandex — which is what makes the
    // flow finish with this tab closed.
    const progress = await api.cloudLinkStatus();
    expect(progress.state).toBe('WAITING_USER');
    for (const call of calls) expect(call.url).toContain('192.168.1.74');
  });

  it('never asks the controller to upload to a path a client chose', async () => {
    await api.retryCloudJob(17);
    const post = calls.find((c) => c.url.includes('/cloud/queue/retry'))!;
    // By id only.  A path or an upload URL from a client would be an
    // instruction about where somebody's measurements get written.
    expect(post.body).toEqual({ jobId: 17 });
  });

  it('distinguishes queued from stored, and says which', async () => {
    reply = () => ({
      status: 200,
      body: {
        ...idle(),
        state: 'UPLOADING',
        queue: {
          paused: false, corrupt: false, pending: 2, failed: 1, acknowledged: 3,
          jobs: [
            { id: 17, sessionId: 'log_0001', segmentId: 'p000001', bytes: 101824,
              state: 'UPLOADING', attempts: 1, nextAttemptEpochMs: 0,
              remotePath: 'disk:/LabController/x/log_0001/p1.csv', lastError: null },
            { id: 18, sessionId: 'log_0001', segmentId: 'p000002', bytes: 101824,
              state: 'REMOTE_CONFLICT', attempts: 3, nextAttemptEpochMs: 0,
              remotePath: 'disk:/LabController/x/log_0001/p2.csv',
              lastError: 'a different file already exists at that path' },
          ],
        },
        current: { jobId: 17, file: 'log_0001_p000001.csv', sentBytes: 65536,
                   totalBytes: 101824, attempt: 1 },
      },
    });

    const status = await api.cloud();
    // "pending" is what is STILL ONLY ON THE DEVICE.  A number that stops going
    // down is the warning that data is not leaving.
    expect(status.queue.pending).toBe(2);
    expect(status.queue.failed).toBe(1);
    expect(status.current?.sentBytes).toBe(65536);
    // A conflict carries its reason, so the operator is not left guessing why
    // one file stopped while others went.
    const conflicted = status.queue.jobs.find((j) => j.state === 'REMOTE_CONFLICT');
    expect(conflicted?.lastError).toContain('already exists');
  });

  it('reports the gates the uploader is waiting on', async () => {
    reply = () => ({
      status: 200,
      body: { ...idle(), state: 'WAITING_NETWORK', networkReady: false,
              queue: { paused: false, corrupt: false, pending: 4, failed: 0,
                       acknowledged: 0, jobs: [] } },
    });
    const status = await api.cloud();
    // Four files waiting and no network is a different situation from four
    // files waiting and a broken account — the page needs to be able to say so.
    expect(status.networkReady).toBe(false);
    expect(status.timeReady).toBe(true);
    expect(status.queue.pending).toBe(4);
  });

  it('surfaces a corrupt queue rather than showing an empty one', async () => {
    reply = () => ({
      status: 200,
      body: { ...idle(), state: 'BLOCKED',
              queue: { paused: false, corrupt: true,
                       error: 'the upload queue could not be read',
                       pending: 0, failed: 0, acknowledged: 0, jobs: [] } },
    });
    const status = await api.cloud();
    // pending is 0 here and that must NOT read as "everything was sent": the
    // corrupt flag is the difference between finished and unknown.
    expect(status.queue.corrupt).toBe(true);
    expect(status.state).toBe('BLOCKED');
  });

  it('uses the right verb for every cloud route', async () => {
    reply = () => ({ status: 200, body: { disconnected: true,
                     revokedRemotely: false, note: 'x', paused: true } });
    await api.cloudQueue();
    await api.pauseCloudQueue(true);
    await api.testCloudAccess();
    await api.disconnectCloud('bench-password');

    expect(calls.map((c) => `${c.method} ${c.url.replace(/^.*\/api\/v1/, '')}`))
      .toEqual([
        'GET /cloud/queue',
        'POST /cloud/queue/pause',
        'POST /cloud/yandex/test',
        'DELETE /cloud/yandex/credentials',
      ]);
  });
});
