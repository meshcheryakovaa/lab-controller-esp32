// =============================================================================
//  network.test.ts — the house-network client (npm test).
//
//  M16 is the first feature where the browser can put the CONTROLLER out of
//  reach.  These tests are about the two ways that happens and how the client
//  is built to avoid them:
//
//    * a Wi-Fi password escaping into somewhere it can be read, and
//    * the interface believing a connection attempt has finished when it has
//      only been accepted.
//
//  Everything here runs against a fake fetch, because what is being checked is
//  the contract with the firmware — which requests go out, and what is done
//  with what comes back — and none of that needs a radio.
// =============================================================================

import { beforeEach, describe, expect, it, vi } from 'vitest';
import { api } from './api';
import type { NetworkStatus } from './types';

interface Call { method: string; url: string; body: unknown }

let calls: Call[] = [];
let reply: (call: Call) => { status: number; body: unknown };

function connected(): NetworkStatus {
  return {
    state: 'STA_CONNECTED',
    configured: true,
    ssid: 'HomeWiFi',
    password_set: true,
    hostname: 'lab-controller-a1b2c3',
    mdns: 'lab-controller-a1b2c3.local',
    pending: false,
    station: { connected: true, ip: '192.168.1.74', rssi: -57 },
    access_point: { active: false, ssid: 'LAB-CONTROLLER-A1B2C3', ip: '' },
    reconnects: 2,
    disconnects: 1,
    last_disconnect_reason: 'BEACON_TIMEOUT',
    last_error: null,
  };
}

beforeEach(() => {
  calls = [];
  reply = () => ({ status: 200, body: {} });
  // api.ts resolves relative paths against location.origin, because in the real
  // app it is served BY the controller.  These tests run in node.
  vi.stubGlobal('location', { origin: 'http://192.168.4.1' });
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

describe('the network client', () => {
  it('sends the password once and never stores or returns it', async () => {
    reply = (call) => {
      if (call.method === 'POST') {
        return { status: 202, body: { accepted: true, state: 'STA_CONNECTING' } };
      }
      return { status: 200, body: connected() };
    };

    await api.connectNetwork('HomeWiFi', 'super-secret-8');

    // It went out exactly once, in the body of the connect request.
    const posts = calls.filter((c) => c.method === 'POST');
    expect(posts).toHaveLength(1);
    expect(posts[0]!.body).toEqual({ ssid: 'HomeWiFi', password: 'super-secret-8' });
    // And never in a URL, where it would land in a proxy log or browser history.
    for (const call of calls) expect(call.url).not.toContain('super-secret-8');

    // What comes back has nowhere to put a password.  This is the type-level
    // guarantee made concrete: reading the status back gets no secret.
    const status = await api.network();
    expect(JSON.stringify(status)).not.toContain('super-secret-8');
    expect(status.password_set).toBe(true);
    expect('password' in status).toBe(false);
  });

  it('treats a connect as accepted, not finished', async () => {
    // 202 with pending:true.  A client that read this as success would tell the
    // operator they are connected while the join is still being attempted — and
    // then show a stale address when it fails.
    reply = () => ({ status: 202, body: { accepted: true, state: 'STA_CONNECTING' } });
    const accepted = await api.connectNetwork('HomeWiFi', 'good-password');
    expect(accepted.accepted).toBe(true);
    expect(accepted.state).toBe('STA_CONNECTING');

    reply = () => ({
      status: 200,
      body: { ...connected(), state: 'STA_CONNECTING', pending: true,
              station: { connected: false, ip: '', rssi: 0 } },
    });
    const midway = await api.network();
    expect(midway.pending).toBe(true);
    expect(midway.station.connected).toBe(false);
    // No address is offered while the attempt is in flight.
    expect(midway.station.ip).toBe('');
  });

  it('keeps the fallback address reachable when a join fails', async () => {
    // A wrong password must leave the access point up and the stored network
    // untouched — this is ADR-0022 as the client sees it.
    reply = () => ({
      status: 200,
      body: {
        ...connected(),
        state: 'AP_STA_FALLBACK',
        pending: false,
        station: { connected: false, ip: '', rssi: 0 },
        access_point: { active: true, ssid: 'LAB-CONTROLLER-A1B2C3',
                        ip: '192.168.4.1' },
        last_error: { code: 'TIMEOUT', detail: 'could not join that network' },
      },
    });

    const status = await api.network();
    expect(status.access_point.active).toBe(true);
    expect(status.access_point.ip).toBe('192.168.4.1');
    // The previously working network is still configured: nothing was replaced
    // by credentials that were never proved.
    expect(status.configured).toBe(true);
    expect(status.ssid).toBe('HomeWiFi');
    expect(status.last_error?.code).toBe('TIMEOUT');
  });

  it('offers the IP alongside the .local name, never only the name', async () => {
    reply = () => ({ status: 200, body: connected() });
    const status = await api.network();
    // Plenty of machines cannot resolve mDNS.  An instrument that only told you
    // a name you cannot reach would be unreachable in practice.
    expect(status.station.ip).toBe('192.168.1.74');
    expect(status.mdns).toBe('lab-controller-a1b2c3.local');
  });

  it('uses the right verb for each network route', async () => {
    reply = (call) => {
      if (call.url.endsWith('/network/config')) {
        return { status: 200, body: { cleared: true, state: 'AP_ONLY',
                                      ip: '192.168.4.1', ssid: 'LAB-CONTROLLER' } };
      }
      if (call.url.endsWith('/network/hostname')) {
        return { status: 200, body: { hostname: 'lab-reactor' } };
      }
      return { status: 200, body: { state: 'COMPLETE', networks: [] } };
    };

    await api.forgetNetwork();
    await api.setHostname('lab-reactor');
    await api.startNetworkScan();
    await api.networkScan();

    expect(calls.map((c) => `${c.method} ${c.url.replace(/^.*\/api\/v1/, '')}`))
      .toEqual([
        'DELETE /network/config',
        'PUT /network/hostname',
        'POST /network/scan',
        'GET /network/scan',
      ]);
  });

  it('reports where to go after the home network is forgotten', async () => {
    reply = () => ({
      status: 200,
      body: { cleared: true, state: 'AP_ONLY', ip: '192.168.4.1',
              ssid: 'LAB-CONTROLLER-A1B2C3' },
    });
    const result = await api.forgetNetwork();
    // Telling the operator the old address is gone without saying where the new
    // one is would be the same as making the instrument unreachable.
    expect(result.ip).toBe('192.168.4.1');
    expect(result.ssid).toBe('LAB-CONTROLLER-A1B2C3');
    expect(result.state).toBe('AP_ONLY');
  });
});
