import { memo, useCallback, useEffect, useMemo, useRef, useState, useSyncExternalStore } from 'react';
import { advanceMeterBar, litMeterSegments, MeterStore } from './meter';

type View = 'mix' | 'talkback' | 'channels' | 'devices' | 'profiles';
type ControlName =
  | 'input.trim'
  | 'input.enabled'
  | 'route.volume'
  | 'route.pan'
  | 'route.enabled'
  | 'route.solo'
  | 'bus.volume'
  | 'bus.enabled'
  | 'talkback.enabled'
  | 'talkback.input'
  | 'talkback.dim'
  | 'talkback.bus'
  | 'listenback.enabled'
  | 'listenback.input'
  | 'listenback.dim'
  | 'listenback.bus'
  | 'meter.bus';
type PairControlName =
  | 'input.trim'
  | 'input.enabled'
  | 'route.enabled'
  | 'route.solo'
  | 'pair.gain'
  | 'pair.balance'
  | 'pair.width'
  | 'pair.pan';

interface Route {
  volume: number;
  pan: number;
  enabled: boolean;
  solo: boolean;
}

interface StereoControls {
  gain: number;
  balance: number;
  width: number;
  pan: number;
}

interface InputChannel {
  index: number;
  label: string;
  available: boolean | null;
  trim: number;
  enabled: boolean;
  route: Route;
  stereo: StereoControls | null;
}

interface InputUpdate {
  index: number;
  trim: number;
  enabled: boolean;
  route?: Route;
  stereo?: StereoControls | null;
}

interface Feature {
  enabled: boolean;
  input: number;
  dim: number;
  buses: boolean[];
}

interface MixerStatus {
  bus: {
    index: number;
    label: string;
    available: boolean | null;
    volume: number;
    enabled: boolean;
  };
  inputs: InputChannel[];
  talkback: Feature;
  listenback: Feature;
  meter: { bus: number };
  topology: {
    known: boolean;
    input_labels: string[];
    input_short_labels: string[];
    outputs: (boolean | null)[];
    ports: { index: number; name: string; configured: boolean | null }[];
  };
}

interface Topology {
  known: boolean;
  input_labels: string[];
  input_short_labels: string[];
  inputs: (boolean | null)[];
  outputs: (boolean | null)[];
  ports: { index: number; name: string; configured: boolean | null }[];
}

interface MeterFrame {
  input_bank: number;
  bus: number;
  values: number[];
  sequence: number;
  sampled_at: number;
}

interface ChannelNames {
  inputs: string[];
  outputs: string[];
}

interface DeviceSetting {
  name: string;
  index: number;
  label: string;
  kind: 'boolean' | 'integer' | 'enumerated';
  writable: boolean;
  values: number[];
  min: number;
  max: number;
  step: number;
  options: { value: number; label: string }[];
  note?: string;
  clear_only?: boolean;
}

interface HardwareDevice {
  name: string;
  port: number | null;
  configured: boolean | null;
  controls: DeviceSetting[];
}

const EMPTY_CHANNEL_NAMES: ChannelNames = {
  inputs: Array(96).fill(''),
  outputs: Array(48).fill(''),
};
const EMPTY_STEREO_PAIRS = Array(48).fill(false) as boolean[];
const EMPTY_HIDDEN_INPUTS = Array(96).fill(false) as boolean[];
// The DSP reports |24-bit sample| >> 8, so a full-scale sample reads 32767.
const METER_FULL_SCALE = 32768;
const METER_CLIP = 32767;
const TRIM_UNITY = 64;
const BRIDGE_LABEL_BELOW_MAX = 18;

interface ProfileSnapshot {
  inputs: { trim: number; enabled: boolean }[];
  buses: {
    volume: number;
    enabled: boolean;
    routes: Route[];
    stereo: { balance: number; width: number }[];
  }[];
  talkback: Feature;
  listenback: Feature;
}

interface ProfileSummary {
  id: string;
  name: string;
  updated: number;
}

interface ProfileDetail extends ProfileSummary {
  changed: number;
  snapshot: ProfileSnapshot;
}

interface Session {
  authenticated: boolean;
  csrf: string | null;
  auth_disabled?: boolean;
}

class RequestError extends Error {
  status: number;

  constructor(message: string, status: number) {
    super(message);
    this.status = status;
  }
}

async function api<T>(
  path: string,
  options: RequestInit = {},
  csrf?: string | null,
): Promise<T> {
  const headers = new Headers(options.headers);
  if (options.body) headers.set('Content-Type', 'application/json');
  if (csrf) headers.set('X-Q-Mix-CSRF', csrf);
  const response = await fetch(path, {
    ...options,
    headers,
    credentials: 'same-origin',
  });
  const payload = (await response.json().catch(() => ({}))) as {
    error?: string;
  };
  if (!response.ok) {
    throw new RequestError(payload.error ?? `Request failed (${response.status})`, response.status);
  }
  return payload as T;
}

function App() {
  const [authenticated, setAuthenticated] = useState(false);
  const [csrf, setCsrf] = useState<string | null>(null);
  const [devNoAuth, setDevNoAuth] = useState(false);
  const [view, setView] = useState<View>('mix');
  const [selectedBus, setSelectedBus] = useState(0);
  const [status, setStatus] = useState<MixerStatus | null>(null);
  const [meterStore] = useState(() => new MeterStore());
  const [meterError, setMeterError] = useState('');
  const [inputLabels, setInputLabels] = useState<string[]>(Array(96).fill(''));
  const [inputPorts, setInputPorts] = useState<Topology['ports']>([]);
  const [channelNames, setChannelNames] = useState<ChannelNames | null>(null);
  const [stereoPairs, setStereoPairs] = useState<boolean[]>(EMPTY_STEREO_PAIRS);
  const [hiddenInputs, setHiddenInputs] = useState<boolean[]>(EMPTY_HIDDEN_INPUTS);
  const [pairBusy, setPairBusy] = useState<number | null>(null);
  const [profiles, setProfiles] = useState<ProfileSummary[]>([]);
  const [selectedProfile, setSelectedProfile] = useState<ProfileDetail | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [pairing, setPairing] = useState(false);
  const selectedBusRef = useRef(selectedBus);
  selectedBusRef.current = selectedBus;

  const loadStatus = useCallback(async (bus = selectedBus) => {
    const next = await api<MixerStatus>(`/api/status?bus=${bus}`);
    // A reply for a bus the user has already left must not replace the view.
    if (bus !== selectedBusRef.current) return;
    setInputLabels(next.topology.input_short_labels);
    setInputPorts(next.topology.ports);
    setStatus(next);
  }, [selectedBus]);

  const loadProfiles = useCallback(async () => {
    const result = await api<{ profiles: ProfileSummary[] }>('/api/profiles');
    setProfiles(result.profiles);
  }, []);

  const loadChannelNames = useCallback(async () => {
    const names = await api<ChannelNames>('/api/channel-names');
    setChannelNames(names);
  }, []);

  const loadStereoPairs = useCallback(async () => {
    const result = await api<unknown>('/api/stereo-pairs');
    if (!isFlagList(result, 'pairs', 48)) {
      throw new Error('The server returned invalid stereo-pair settings.');
    }
    setStereoPairs((current) => sameFlags(current, result.pairs) ? current : result.pairs);
  }, []);

  const loadHiddenInputs = useCallback(async () => {
    const result = await api<unknown>('/api/hidden-inputs');
    if (!isFlagList(result, 'hidden', 96)) {
      throw new Error('The server returned invalid hidden-input settings.');
    }
    setHiddenInputs((current) => sameFlags(current, result.hidden) ? current : result.hidden);
  }, []);

  useEffect(() => {
    let active = true;
    api<Session>('/api/session')
      .then((session) => {
        if (!active) return;
        setAuthenticated(session.authenticated);
        setCsrf(session.csrf);
        setDevNoAuth(session.auth_disabled ?? false);
      })
      .catch((cause: Error) => {
        if (active) setError(cause.message);
      });
    return () => {
      active = false;
    };
  }, []);

  useEffect(() => {
    if (!authenticated) return;
    setBusy(true);
    Promise.all([loadStatus(selectedBus), loadProfiles()])
      .catch((cause: Error) => setError(cause.message))
      .finally(() => setBusy(false));
  }, [authenticated, loadProfiles, loadStatus, selectedBus]);

  useEffect(() => {
    if (!authenticated) return;
    loadChannelNames().catch((cause: Error) => setError(cause.message));
  }, [authenticated, loadChannelNames]);

  useEffect(() => {
    if (!authenticated) return;
    let active = true;
    const refresh = () => {
      Promise.all([loadStereoPairs(), loadHiddenInputs()]).catch((cause: Error) => {
        if (active) setError(cause.message);
      });
    };
    refresh();
    const timer = window.setInterval(refresh, 3000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, [authenticated, loadHiddenInputs, loadStereoPairs]);

  // Linking changes the strips' stereo controls, also when another browser does it.
  const linkedPairsLoaded = useRef(stereoPairs);
  useEffect(() => {
    if (!authenticated || linkedPairsLoaded.current === stereoPairs) return;
    linkedPairsLoaded.current = stereoPairs;
    loadStatus().catch((cause: Error) => setError(cause.message));
  }, [authenticated, loadStatus, stereoPairs]);

  useEffect(() => {
    if (!authenticated || view !== 'mix') return;
    const stream = new EventSource('/api/meters/stream');
    meterStore.reset();
    setMeterError('');
    stream.onopen = () => setMeterError('');
    stream.onerror = () => {
      setMeterError('Meter stream disconnected; reconnecting…');
    };
    stream.addEventListener('meters', (event) => {
      let payload: unknown;
      try {
        payload = JSON.parse((event as MessageEvent<string>).data) as unknown;
      } catch {
        setMeterError('The meter stream sent invalid data.');
        return;
      }

      if (typeof payload !== 'object' || payload === null) {
        setMeterError('The meter stream sent invalid data.');
        return;
      }
      if ('error' in payload) {
        setMeterError(
          typeof payload.error === 'string'
            ? `Meter read failed: ${payload.error}`
            : 'The meter stream sent an invalid error.',
        );
        return;
      }
      if (!isMeterFrame(payload)) {
        setMeterError('The meter stream sent invalid data.');
        return;
      }

      setMeterError('');
      meterStore.publish(payload.input_bank, payload.bus, payload.values, payload.sequence);
    });
    return () => {
      stream.close();
    };
  }, [authenticated, view, meterStore]);

  useEffect(() => {
    if (!authenticated) return;
    let active = true;
    const refresh = async () => {
      try {
        const topology = await api<Topology>('/api/topology');
        if (!active) return;
        setInputLabels(topology.input_short_labels);
        setInputPorts(topology.ports);
        setStatus((current) => current ? {
          ...current,
          topology,
          bus: {
            ...current.bus,
            available: topology.outputs[current.bus.index],
          },
          inputs: current.inputs.map((input) => ({
            ...input,
            available: topology.inputs[input.index],
            label: topology.input_labels[input.index],
          })),
        } : current);
      } catch (cause) {
        if (active) setError((cause as Error).message);
      }
    };
    void refresh();
    const timer = window.setInterval(refresh, 5000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, [authenticated]);

  const performControl = useCallback(
    async (
      control: ControlName,
      value: number,
      bus?: number,
      input?: number,
    ): Promise<void> => {
      setError('');
      try {
        const result = await api<{ value: number; inputs?: InputUpdate[] }>(
          '/api/control',
          {
            method: 'POST',
            body: JSON.stringify({ control, value, bus, input }),
          },
          csrf,
        );
        setStatus((current) => {
          if (!current) return current;
          return result.inputs
            ? mergeInputs(current, result.inputs, bus)
            : patchStatus(current, control, result.value, bus, input);
        });
      } catch (cause) {
        setError((cause as Error).message);
        try {
          await loadStatus(selectedBus);
        } catch (refreshError) {
          setError(`${(cause as Error).message}. Readback failed: ${(refreshError as Error).message}`);
        }
      }
    },
    [csrf, loadStatus, selectedBus],
  );

  const performPairControl = useCallback(
    async (
      control: PairControlName,
      value: number,
      bus: number | undefined,
      leftInput: number,
    ) => {
      setError('');
      try {
        const result = await api<{ inputs: InputUpdate[] }>(
          '/api/stereo-pairs/control',
          {
            method: 'POST',
            body: JSON.stringify({ control, value, bus, left_input: leftInput }),
          },
          csrf,
        );
        setStatus((current) =>
          current ? mergeInputs(current, result.inputs, bus) : current,
        );
      } catch (cause) {
        setError((cause as Error).message);
        try {
          await loadStatus(selectedBus);
        } catch (refreshError) {
          setError(`${(cause as Error).message}. Readback failed: ${(refreshError as Error).message}`);
        }
      }
    },
    [csrf, loadStatus, selectedBus],
  );

  const changeStereoPair = useCallback(
    async (leftInput: number, enabled: boolean) => {
      setPairBusy(leftInput);
      setError('');
      try {
        const result = await api<unknown>(
          '/api/stereo-pairs',
          {
            method: 'POST',
            body: JSON.stringify({ left_input: leftInput, enabled }),
          },
          csrf,
        );
        if (!isFlagList(result, 'pairs', 48)) {
          throw new Error('The server returned invalid stereo-pair settings.');
        }
        linkedPairsLoaded.current = result.pairs;
        setStereoPairs(result.pairs);
        await loadStatus(selectedBus);
      } catch (cause) {
        setError((cause as Error).message);
        try {
          await loadStatus(selectedBus);
        } catch (refreshError) {
          setError(`${(cause as Error).message}. Readback failed: ${(refreshError as Error).message}`);
        }
      } finally {
        setPairBusy(null);
      }
    },
    [csrf, loadStatus, selectedBus],
  );

  const changeHiddenInputs = useCallback(
    async (inputs: number[], hidden: boolean) => {
      setError('');
      try {
        const result = await api<unknown>(
          '/api/hidden-inputs',
          { method: 'POST', body: JSON.stringify({ inputs, hidden }) },
          csrf,
        );
        if (!isFlagList(result, 'hidden', 96)) {
          throw new Error('The server returned invalid hidden-input settings.');
        }
        setHiddenInputs(result.hidden);
      } catch (cause) {
        setError((cause as Error).message);
      }
    },
    [csrf],
  );

  const pair = async (code: string) => {
    setPairing(true);
    setError('');
    try {
      const session = await api<Session>(
        '/api/pair',
        { method: 'POST', body: JSON.stringify({ code }) },
      );
      setAuthenticated(true);
      setCsrf(session.csrf);
      setDevNoAuth(session.auth_disabled ?? false);
    } catch (cause) {
      setError((cause as Error).message);
    } finally {
      setPairing(false);
    }
  };

  const logout = async () => {
    if (devNoAuth) return;
    try {
      await api('/api/logout', { method: 'POST', body: '{}' }, csrf);
      setAuthenticated(false);
      setCsrf(null);
      setDevNoAuth(false);
      setStatus(null);
      setChannelNames(null);
    } catch (cause) {
      setError((cause as Error).message);
    }
  };

  if (!authenticated) {
    return (
      <PairScreen
        busy={pairing}
        error={error}
        onPair={pair}
      />
    );
  }

  return (
    <div className="app-shell">
      <Header
        active={view}
        onNavigate={setView}
        onLogout={logout}
        connected={!!status}
        devNoAuth={devNoAuth}
      />
      {error && (
        <div className="alert alert-error" role="alert">
          <span className="alert-mark">!</span>
          <span>{error}</span>
          <button className="icon-button alert-close" onClick={() => setError('')} aria-label="Dismiss error">
            ×
          </button>
        </div>
      )}
      {view === 'mix' && (
        <MixView
          status={status}
          meterStore={meterStore}
          meterError={meterError}
          inputLabels={inputLabels}
          inputPorts={inputPorts}
          channelNames={channelNames ?? EMPTY_CHANNEL_NAMES}
          stereoPairs={stereoPairs}
          hiddenInputs={hiddenInputs}
          pairBusy={pairBusy}
          selectedBus={selectedBus}
          busy={busy}
          onBusChange={(bus) => {
            if (bus === selectedBus) return;
            setStatus(null);
            setSelectedBus(bus);
          }}
          onControl={performControl}
          onPairChange={changeStereoPair}
          onPairControl={performPairControl}
          onHiddenChange={changeHiddenInputs}
        />
      )}
      {view === 'talkback' && (
        <TalkbackView
          status={status}
          channelNames={channelNames ?? EMPTY_CHANNEL_NAMES}
          onControl={performControl}
        />
      )}
      {view === 'channels' && (
        channelNames ? (
          <ChannelNamesView
            names={channelNames}
            status={status}
            busy={busy}
            onSave={async (names) => {
              setBusy(true);
              setError('');
              try {
                const saved = await api<ChannelNames>(
                  '/api/channel-names',
                  { method: 'POST', body: JSON.stringify(names) },
                  csrf,
                );
                setChannelNames(saved);
              } catch (cause) {
                setError((cause as Error).message);
              } finally {
                setBusy(false);
              }
            }}
          />
        ) : (
          <main className="single-stage">
            <div className="loading-panel panel"><div className="spinner" /><span>Loading channel names…</span></div>
          </main>
        )
      )}
      {view === 'devices' && <DevicesView csrf={csrf} onError={setError} />}
      {view === 'profiles' && (
        <ProfilesView
          profiles={profiles}
          selected={selectedProfile}
          status={status}
          channelNames={channelNames ?? EMPTY_CHANNEL_NAMES}
          selectedBus={selectedBus}
          busy={busy}
          onSelect={async (id) => {
            setBusy(true);
            setError('');
            try {
              const profile = await api<ProfileDetail>(`/api/profiles/${id}`);
              setSelectedProfile(profile);
            } catch (cause) {
              setError((cause as Error).message);
            } finally {
              setBusy(false);
            }
          }}
          onCreate={async (name) => {
            setBusy(true);
            setError('');
            try {
              const created = await api<ProfileSummary>(
                '/api/profiles',
                { method: 'POST', body: JSON.stringify({ name }) },
                csrf,
              );
              await loadProfiles();
              const detail = await api<ProfileDetail>(`/api/profiles/${created.id}`);
              setSelectedProfile(detail);
            } catch (cause) {
              setError((cause as Error).message);
            } finally {
              setBusy(false);
            }
          }}
          onApply={async () => {
            if (!selectedProfile) return;
            const confirmed = window.confirm(
              `Apply “${selectedProfile.name}” to the live mixer? Outputs are muted while the profile is applied.`,
            );
            if (!confirmed) return;
            setBusy(true);
            setError('');
            try {
              await api<{ applied: number }>(
                `/api/profiles/${selectedProfile.id}/apply`,
                { method: 'POST', body: '{}', headers: { 'Content-Type': 'application/json' } },
                csrf,
              );
              await loadStatus(selectedBus);
              setSelectedProfile(await api<ProfileDetail>(`/api/profiles/${selectedProfile.id}`));
            } catch (cause) {
              setError((cause as Error).message);
              try {
                await loadStatus(selectedBus);
              } catch (refreshError) {
                setError(`${(cause as Error).message}. Readback failed: ${(refreshError as Error).message}`);
              }
            } finally {
              setBusy(false);
            }
          }}
          onDelete={async (id) => {
            if (!window.confirm('Delete this saved profile?')) return;
            try {
              await api(`/api/profiles/${id}`, { method: 'DELETE' }, csrf);
              await loadProfiles();
              if (selectedProfile?.id === id) setSelectedProfile(null);
            } catch (cause) {
              setError((cause as Error).message);
            }
          }}
          onReplace={async (id) => {
            setBusy(true);
            setError('');
            try {
              await api(
                `/api/profiles/${id}`,
                { method: 'PUT', body: '{}' },
                csrf,
              );
              await loadProfiles();
              setSelectedProfile(await api<ProfileDetail>(`/api/profiles/${id}`));
            } catch (cause) {
              setError((cause as Error).message);
            } finally {
              setBusy(false);
            }
          }}
        />
      )}
    </div>
  );
}

function Header({
  active,
  onNavigate,
  onLogout,
  connected,
  devNoAuth,
}: {
  active: View;
  onNavigate: (view: View) => void;
  onLogout: () => void;
  connected: boolean;
  devNoAuth: boolean;
}) {
  return (
    <header className="topbar">
      <div className="brand">
        <div className="brand-mark" aria-hidden="true">
          <span />
          <span />
          <span />
        </div>
        <div>
          <div className="brand-name">Q-<span>MIX</span></div>
          <div className="brand-subtitle">MOTU PCI-424</div>
        </div>
      </div>
      <nav className="primary-nav" aria-label="Main navigation">
        <NavButton active={active === 'mix'} onClick={() => onNavigate('mix')}>Mix</NavButton>
        <NavButton active={active === 'talkback'} onClick={() => onNavigate('talkback')}>Talkback</NavButton>
        <NavButton active={active === 'channels'} onClick={() => onNavigate('channels')}>Channels</NavButton>
        <NavButton active={active === 'devices'} onClick={() => onNavigate('devices')}>Devices</NavButton>
        <NavButton active={active === 'profiles'} onClick={() => onNavigate('profiles')}>Profiles</NavButton>
      </nav>
      <div className="topbar-right">
        {devNoAuth && (
          <span
            className="dev-mode-badge"
            title="Authentication is disabled; any allowed network client can control the mixer."
          >
            DEV · NO AUTH
          </span>
        )}
        <div className="connection-state">
          <span className={`status-dot ${connected ? 'is-on' : ''}`} />
          <span>{connected ? 'ONLINE' : 'CONNECTING'}</span>
        </div>
        {!devNoAuth && (
          <button className="icon-button logout-button" onClick={onLogout} aria-label="Sign out" title="Sign out">
            <LogoutIcon />
          </button>
        )}
      </div>
    </header>
  );
}

function NavButton({
  active,
  onClick,
  children,
}: {
  active: boolean;
  onClick: () => void;
  children: React.ReactNode;
}) {
  return (
    <button className={`nav-button ${active ? 'is-active' : ''}`} onClick={onClick}>
      {children}
    </button>
  );
}

function PairScreen({
  busy,
  error,
  onPair,
}: {
  busy: boolean;
  error: string;
  onPair: (code: string) => void;
}) {
  const [code, setCode] = useState('');
  return (
    <main className="pair-screen">
      <section className="pair-card">
        <div className="brand-mark pair-mark" aria-hidden="true">
          <span />
          <span />
          <span />
        </div>
        <p className="eyebrow">MOTU PCI-424 · LOCAL CONTROL</p>
        <h1>Your mix,<br /><span>in reach.</span></h1>
        <p className="pair-copy">
          Enter the pairing code printed by the Q-Mix service. For a systemd
          user service, find it in the service journal.
        </p>
        <form
          onSubmit={(event) => {
            event.preventDefault();
            onPair(code);
          }}
        >
          <label className="field-label" htmlFor="pair-code">Pairing code</label>
          <input
            id="pair-code"
            autoComplete="one-time-code"
            autoCapitalize="off"
            spellCheck={false}
            value={code}
            onChange={(event) => setCode(event.target.value)}
            placeholder="Paste code from mixer host"
            required
          />
          {error && <p className="form-error" role="alert">{error}</p>}
          <button className="button button-primary button-wide" disabled={busy || !code.trim()}>
            {busy ? 'Connecting…' : 'Connect device'}
            <ArrowIcon />
          </button>
        </form>
        <div className="pair-footnote">
          <LockIcon />
          <span>Private to your local network</span>
        </div>
      </section>
    </main>
  );
}

function MixView({
  status,
  meterStore,
  meterError,
  inputLabels,
  inputPorts,
  channelNames,
  stereoPairs,
  hiddenInputs,
  pairBusy,
  selectedBus,
  busy,
  onBusChange,
  onControl,
  onPairChange,
  onPairControl,
  onHiddenChange,
}: {
  status: MixerStatus | null;
  meterStore: MeterStore;
  meterError: string;
  inputLabels: string[];
  inputPorts: Topology['ports'];
  channelNames: ChannelNames;
  stereoPairs: boolean[];
  hiddenInputs: boolean[];
  pairBusy: number | null;
  selectedBus: number;
  busy: boolean;
  onBusChange: (bus: number) => void;
  onControl: (
    control: ControlName,
    value: number,
    bus?: number,
    input?: number,
  ) => Promise<void>;
  onPairChange: (leftInput: number, enabled: boolean) => Promise<void>;
  onPairControl: (
    control: PairControlName,
    value: number,
    bus: number | undefined,
    leftInput: number,
  ) => Promise<void>;
  onHiddenChange: (inputs: number[], hidden: boolean) => Promise<void>;
}) {
  const [search, setSearch] = useState('');
  const [connectedOnly, setConnectedOnly] = useState(false);
  const [showHidden, setShowHidden] = useState(false);
  const [bridgeExpanded, setBridgeExpanded] = useState(true);
  const searchRef = useRef<HTMLInputElement>(null);
  const meterSelectionAttempt = useRef<number | null>(null);
  useEffect(() => {
    const focusSearch = (event: KeyboardEvent) => {
      if (
        event.key === '/'
        && !event.metaKey
        && !event.ctrlKey
        && !event.altKey
        && !['INPUT', 'TEXTAREA', 'SELECT'].includes(
          (event.target as HTMLElement | null)?.tagName ?? '',
        )
      ) {
        event.preventDefault();
        searchRef.current?.focus();
      }
      if (event.key === 'Escape' && document.activeElement === searchRef.current) {
        setSearch('');
        searchRef.current?.blur();
      }
    };
    window.addEventListener('keydown', focusSearch);
    return () => window.removeEventListener('keydown', focusSearch);
  }, []);
  const { visibleStrips, hiddenCount } = useMemo(() => {
    const strips: { input: InputChannel; partner: InputChannel | null }[] = [];
    let hiddenCount = 0;
    if (!status) return { visibleStrips: strips, hiddenCount };
    const query = search.trim().toLowerCase();
    const matches = (input: InputChannel) =>
      !query
      || input.label.toLowerCase().includes(query)
      || channelNames.inputs[input.index].toLowerCase().includes(query)
      || String(input.index + 1).includes(query);
    for (let index = 0; index < status.inputs.length; index += 1) {
      const input = status.inputs[index];
      // A pair shows as one strip once the mixer reports its stereo controls.
      const partner = index % 2 === 0 && stereoPairs[index / 2] && input.stereo
        ? status.inputs[index + 1]
        : null;
      if (partner) index += 1;
      if (hiddenInputs[input.index]) {
        hiddenCount += 1;
        if (!showHidden) continue;
      }
      const connected = input.available === true || partner?.available === true;
      if (
        (!connectedOnly || connected)
        && (matches(input) || (partner !== null && matches(partner)))
      ) {
        strips.push({ input, partner });
      }
    }
    return { visibleStrips: strips, hiddenCount };
  }, [status, search, connectedOnly, channelNames, stereoPairs, hiddenInputs, showHidden]);
  const bridgeLabels = useMemo(
    () => Array.from({ length: 96 }, (_, index) =>
      channelNames.inputs[index]?.trim() || inputLabels[index]?.trim() || ''),
    [channelNames, inputLabels],
  );
  // Longer labels sit beside their meter instead of taking height from every strip.
  const bridgeLabelHeight = Math.ceil(
    (Math.max(0, ...bridgeLabels.filter((label) => label.length <= BRIDGE_LABEL_BELOW_MAX).map((label) => label.length)) + 2) * 4.4 + 8,
  );

  useEffect(() => {
    if (!status) return;
    if (status.meter.bus === selectedBus) {
      meterSelectionAttempt.current = selectedBus;
      return;
    }
    if (meterSelectionAttempt.current === selectedBus) return;
    meterSelectionAttempt.current = selectedBus;
    void onControl('meter.bus', selectedBus);
  }, [status?.meter.bus, selectedBus, onControl]);

  return (
    <main
      className={`console-layout ${bridgeExpanded ? '' : 'is-bridge-collapsed'}`}
      style={{ '--bridge-label-height': `${bridgeLabelHeight}px` } as React.CSSProperties}
    >
      <aside className="bus-rail console-bus-rail">
        <div className="rail-heading">
          <span className="eyebrow">OUTPUTS</span>
          <span className="rail-count">48</span>
        </div>
        {!status && <div className="skeleton rail-skeleton" />}
        {status?.topology.ports.map((port) => (
          <section className="port-group" key={port.index}>
            <div className="port-label">
              <span className={`mini-dot ${port.configured ? 'is-on' : ''}`} />
              <span>PORT {String(port.index + 1).padStart(2, '0')}</span>
              <span className="port-name">{port.name}</span>
            </div>
            <div className="bus-grid">
              {Array.from({ length: 12 }, (_, pair) => {
                const index = port.index * 12 + pair;
                const available = status.topology.outputs[index];
                const identifier = `Bus ${String(index + 1).padStart(2, '0')}`;
                const label = displayChannelName(identifier, channelNames.outputs[index]);
                return (
                  <button
                    key={index}
                    className={`bus-button ${selectedBus === index ? 'is-selected' : ''} ${available === false ? 'is-unavailable' : ''}`}
                    onClick={() => onBusChange(index)}
                    title={label}
                    aria-label={`Select ${label}`}
                    aria-pressed={selectedBus === index}
                  >
                    <span className="bus-index">{String(pair + 1).padStart(2, '0')}</span>
                    <span className="bus-label">
                      {channelNames.outputs[index].trim() || `Output ${String(index + 1).padStart(2, '0')}`}
                    </span>
                    <i />
                  </button>
                );
              })}
            </div>
          </section>
        ))}
      </aside>

      <section className="meter-bridge-panel panel" aria-label="Input meter bridge">
        <div className="console-panel-heading">
          <span className="eyebrow">INPUT METER BRIDGE</span>
          <div className="meter-bridge-heading-actions">
            {bridgeExpanded && <span className="meter-standard">dBFS</span>}
            <button
              type="button"
              className="meter-bridge-toggle"
              aria-label={`${bridgeExpanded ? 'Collapse' : 'Expand'} input meter bridge`}
              aria-expanded={bridgeExpanded}
              aria-controls="input-meter-bridge-content"
              title={`${bridgeExpanded ? 'Collapse' : 'Expand'} input meter bridge`}
              onClick={() => setBridgeExpanded((expanded) => !expanded)}
            >
              <svg viewBox="0 0 16 16" aria-hidden="true">
                <path d="M3 10 8 5l5 5" />
              </svg>
            </button>
          </div>
        </div>
        <div className="meter-bridge-body" id="input-meter-bridge-content" hidden={!bridgeExpanded}>
          <DbScale className="meter-bridge-scale" size="bridge" />
          <div className="meter-bridge-scroll">
            <div className="meter-bridge-devices">
              {Array.from({ length: 4 }, (_, portIndex) => {
                const port = inputPorts.find((item) => item.index === portIndex);
                const name = !port || port.name === `Port ${portIndex}`
                  ? `Port ${String(portIndex + 1).padStart(2, '0')}`
                  : port.name;
                return (
                  <section className="meter-bridge-device" key={portIndex} aria-label={`Port ${portIndex + 1}: ${name}`}>
                    <div className="meter-bridge-columns">
                      {bridgeLabels.slice(portIndex * 24, (portIndex + 1) * 24).map((label, offset) => {
                        const index = portIndex * 24 + offset;
                        return (
                          <BridgeMeter
                            key={index}
                            index={index}
                            label={label}
                            meterStore={meterStore}
                            gain={inputMeterGain(status?.inputs[index])}
                          />
                        );
                      })}
                    </div>
                    <div className="meter-bridge-device-label">{name}</div>
                  </section>
                );
              })}
            </div>
          </div>
        </div>
      </section>

      <section className="input-stage">
        <header className="input-stage-heading">
          <div className="input-stage-title">
            <h1>
              {status
                ? displayChannelName(status.bus.label, channelNames.outputs[status.bus.index])
                : 'Loading mixer…'}
            </h1>
          </div>
          <div className="heading-actions">
            <label className="search-field">
              <SearchIcon />
              <input
                ref={searchRef}
                value={search}
                onChange={(event) => setSearch(event.target.value)}
                placeholder="Find an input"
              />
              <kbd>/</kbd>
            </label>
            <button
              className={`button button-quiet ${connectedOnly ? 'is-toggled' : ''}`}
              onClick={() => setConnectedOnly((value) => !value)}
              aria-pressed={connectedOnly}
              title="Show only inputs reported as present by the driver"
            >
              <span className="mini-dot is-on" />
              Show connected only
            </button>
            <button
              className={`button button-quiet ${showHidden ? 'is-toggled' : ''}`}
              aria-pressed={showHidden}
              onClick={() => setShowHidden((value) => !value)}
            >
              Show hidden
              {hiddenCount > 0 && <span className="button-count">{hiddenCount}</span>}
            </button>
          </div>
        </header>
        <div className="input-strip-scroller">
          {status ? visibleStrips.map(({ input, partner }) => (
            <ChannelCard
              key={input.index}
              input={input}
              partner={partner}
              meterStore={meterStore}
              channelName={channelNames.inputs[input.index]}
              partnerName={partner ? channelNames.inputs[partner.index] : ''}
              pairEnabled={stereoPairs[Math.floor(input.index / 2)]}
              pairBusy={pairBusy === input.index - (input.index % 2)}
              hidden={hiddenInputs[input.index]}
              bus={selectedBus}
              onControl={onControl}
              onPairChange={onPairChange}
              onPairControl={onPairControl}
              onHiddenChange={onHiddenChange}
            />
          )) : (
            <div className="loading-panel panel">
              <div className="spinner" />
              <span>{busy ? 'Reading the mixer…' : 'Connecting to ALSA…'}</span>
            </div>
          )}
          {status && visibleStrips.length === 0 && (
            <div className="empty-state panel">
              <SearchIcon />
              <strong>No inputs found</strong>
            </div>
          )}
        </div>
      </section>

      <BusMasterStrip
        status={status}
        meterStore={meterStore}
        meterError={meterError}
        channelName={channelNames.outputs[selectedBus]}
        selectedBus={selectedBus}
        onControl={onControl}
      />
    </main>
  );
}

const ChannelCard = memo(function ChannelCard({
  input,
  partner,
  meterStore,
  channelName,
  partnerName,
  pairEnabled,
  pairBusy,
  hidden,
  bus,
  onControl,
  onPairChange,
  onPairControl,
  onHiddenChange,
}: {
  input: InputChannel;
  partner: InputChannel | null;
  meterStore: MeterStore;
  channelName: string;
  partnerName: string;
  pairEnabled: boolean;
  pairBusy: boolean;
  hidden: boolean;
  bus: number;
  onControl: (
    control: ControlName,
    value: number,
    bus?: number,
    input?: number,
  ) => Promise<void>;
  onPairChange: (leftInput: number, enabled: boolean) => Promise<void>;
  onPairControl: (
    control: PairControlName,
    value: number,
    bus: number | undefined,
    leftInput: number,
  ) => Promise<void>;
  onHiddenChange: (inputs: number[], hidden: boolean) => Promise<void>;
}) {
  const stereo = partner ? input.stereo : null;
  const pairLeft = input.index - (input.index % 2);
  const pairLabel = `${String(pairLeft + 1).padStart(2, '0')}/${String(pairLeft + 2).padStart(2, '0')}`;
  const number = partner ? pairLabel : String(input.index + 1).padStart(2, '0');
  const label = partner
    ? `${displayChannelName(input.label, channelName)} / ${displayChannelName(partner.label, partnerName)}`
    : displayChannelName(input.label, channelName);
  const setShared = (
    control: 'input.trim' | 'input.enabled' | 'route.enabled' | 'route.solo',
    value: number,
    routeBus?: number,
  ) =>
    partner
      ? onPairControl(control, value, routeBus, pairLeft)
      : onControl(control, value, routeBus, input.index);
  return (
    <article
      className={`channel-card console-channel-strip panel ${partner ? 'is-stereo' : ''} ${input.available === false ? 'is-offline' : ''} ${hidden ? 'is-hidden-strip' : ''}`}
      title={label}
    >
      <header className="strip-header">
        <span className="strip-number">{number}</span>
        <strong className="strip-name">
          {label}
        </strong>
        <div className="strip-header-actions">
          <PowerButton
            active={input.enabled}
            ariaLabel={`Input ${number}`}
            disabled={pairBusy}
            onChange={(value) => void setShared('input.enabled', Number(value))}
          />
          {input.index === pairLeft && (
            <button
              type="button"
              className={`stereo-link-button ${pairEnabled ? 'is-active' : ''}`}
              aria-label={`Stereo link pair ${pairLabel}`}
              aria-pressed={pairEnabled}
              title={pairEnabled ? 'Unlink stereo pair' : 'Link stereo pair'}
              disabled={pairBusy}
              onClick={() => void onPairChange(pairLeft, !pairEnabled)}
            >
              <svg viewBox="0 0 24 16" aria-hidden="true">
                <circle cx={pairEnabled ? 9 : 6} cy="8" r="5" />
                <circle cx={pairEnabled ? 15 : 18} cy="8" r="5" />
              </svg>
            </button>
          )}
        </div>
      </header>

      <ControlSlider
        label="TRIM"
        value={input.trim}
        min={64}
        max={255}
        step={1}
        onCommit={(value) => void setShared('input.trim', value)}
        format={formatTrim}
        compact
      />

      <div className="strip-level-controls">
        <DbScale className="strip-shared-scale" size="strip" />
        <MeterGroup
          meterStore={meterStore}
          indices={partner ? [input.index, partner.index] : [input.index]}
          gains={[inputMeterGain(input), inputMeterGain(partner ?? undefined)]}
          labels={partner
            ? [`Input ${pairLeft + 1}`, `Input ${pairLeft + 2}`]
            : [`Input ${number}`]}
          size="strip"
        />
        <ConsoleFader
          label="SEND"
          value={stereo ? stereo.gain : input.route.volume}
          min={0}
          max={32768}
          step={128}
          format={formatGain}
          onCommit={(value) => void (stereo
            ? onPairControl('pair.gain', value, bus, pairLeft)
            : onControl('route.volume', value, bus, input.index))}
        />
      </div>

      <div className="strip-pan-controls">
        <PanDial
          value={stereo ? stereo.pan : input.route.pan}
          min={stereo ? Math.abs(stereo.width - 64) : 0}
          max={stereo ? 128 - Math.abs(stereo.width - 64) : 128}
          label={`Pan input ${number}`}
          onCommit={(value) => void (stereo
            ? onPairControl('pair.pan', value, bus, pairLeft)
            : onControl('route.pan', value, bus, input.index))}
        />
        {stereo && (
          <div className="strip-stereo-controls">
            <ControlSlider
              label="BAL"
              value={Math.round(stereo.balance)}
              min={0}
              max={128}
              step={1}
              center={64}
              format={formatPan}
              onCommit={(value) => void onPairControl('pair.balance', value, bus, pairLeft)}
              compact
            />
            <ControlSlider
              label="WIDTH"
              value={stereo.width}
              min={64 - Math.min(stereo.pan, 128 - stereo.pan)}
              max={64 + Math.min(stereo.pan, 128 - stereo.pan)}
              step={1}
              center={64}
              format={formatWidth}
              onCommit={(value) => void onPairControl('pair.width', value, bus, pairLeft)}
              compact
            />
          </div>
        )}
      </div>

      <div className="channel-actions strip-actions">
        <EngagedButton
          label="Mute"
          tone="mute"
          engaged={!input.route.enabled}
          ariaLabel={`Mute input ${number}`}
          onChange={(muted) => void setShared('route.enabled', Number(!muted), bus)}
        />
        <EngagedButton
          label="Solo"
          tone="solo"
          engaged={input.route.solo}
          ariaLabel={`Solo input ${number}`}
          onChange={(active) => void setShared('route.solo', Number(active), bus)}
        />
        <EngagedButton
          label="Hide"
          tone="hide"
          engaged={hidden}
          ariaLabel={`Hide input ${number}`}
          onChange={(next) => void onHiddenChange(
            partner ? [input.index, partner.index] : [input.index],
            next,
          )}
        />
      </div>
    </article>
  );
});

const BridgeMeter = memo(function BridgeMeter({
  index,
  label,
  meterStore,
  gain,
}: {
  index: number;
  label: string;
  meterStore: MeterStore;
  gain: number;
}) {
  const channelNumber = String(index + 1).padStart(2, '0');
  return (
    <div className={`bridge-meter-column ${label.length > BRIDGE_LABEL_BELOW_MAX ? 'is-long-label' : ''}`} title={label || `Input ${channelNumber}`}>
      <MeterGroup
        meterStore={meterStore}
        indices={[index]}
        gains={[gain]}
        labels={[label || `Input ${channelNumber}`]}
        size="bridge"
      />
      <span className="bridge-channel-label">
        <strong>{channelNumber}</strong>
        <span>{label}</span>
      </span>
    </div>
  );
});

function MeterGroup({
  meterStore,
  indices,
  kind = 'input',
  bus,
  gains,
  labels,
  size,
  title,
  feet,
}: {
  meterStore: MeterStore;
  indices: number[];
  kind?: 'input' | 'output';
  bus?: number;
  gains?: number[];
  labels: string[];
  size: 'bridge' | 'strip' | 'master';
  title?: string;
  feet?: string[];
}) {
  const [clipped, setClipped] = useState(false);
  const onClip = useCallback(() => setClipped(true), []);
  const name = title ?? labels.join(' and ');
  return (
    <div
      className={`dbfs-meter dbfs-meter--${size}`}
      style={{ '--meter-channels': indices.length } as React.CSSProperties}
      role="group"
      aria-label={`${name} level${clipped ? ', clip held' : ''}`}
    >
      <div className="dbfs-meter-head">
        {title && <span className="dbfs-meter-title">{title}</span>}
        <button
          type="button"
          className={`clip-indicator ${clipped ? 'is-clipped' : ''}`}
          aria-label={`${clipped ? 'Clear' : 'No'} clip indicator for ${name}`}
          disabled={!clipped}
          onClick={() => setClipped(false)}
        >
          CLIP
        </button>
      </div>
      {indices.map((meterIndex, index) => (
        <MeterChannel
          key={index}
          meterStore={meterStore}
          kind={kind}
          index={meterIndex}
          bus={bus}
          gain={gains?.[index] ?? 1}
          column={index + 1}
          foot={feet?.[index]}
          onClip={onClip}
        />
      ))}
    </div>
  );
}

function MeterChannel({
  meterStore,
  kind,
  index,
  bus,
  gain,
  column,
  foot,
  onClip,
}: {
  meterStore: MeterStore;
  kind: 'input' | 'output';
  index: number;
  bus?: number;
  gain: number;
  column: number;
  foot?: string;
  onClip: () => void;
}) {
  const subscribe = useCallback(
    (listener: () => void) => meterStore.subscribe(kind, index, listener),
    [meterStore, kind, index],
  );
  const getSnapshot = useCallback(
    () => meterStore.get(kind, index, bus),
    [meterStore, kind, index, bus],
  );
  const meter = useSyncExternalStore(subscribe, getSnapshot, getSnapshot);
  const holdDurationMs = 5000;
  const decayRateDbPerSecond = 15;
  const [heldDb, setHeldDb] = useState(-60);
  const heldDbRef = useRef(-60);
  const [barDb, setBarDb] = useState(-Infinity);
  const barDbRef = useRef(-Infinity);
  const holdUntilRef = useRef(0);
  const lastSampleAtRef = useRef<number | null>(null);
  const level = meter === null ? 0 : meter.value * gain;
  const currentDb = meter === null ? -Infinity : peakToDbfs(level);
  const displayedDb = meter === null ? -Infinity : Math.max(currentDb, barDb);
  const readoutDb = heldDb <= -60 ? -Infinity : heldDb;

  useEffect(() => {
    if (meter === null) {
      lastSampleAtRef.current = null;
      return;
    }

    const now = performance.now();
    const previousSampleAt = lastSampleAtRef.current;
    lastSampleAtRef.current = now;
    const nextDb = peakToDbfs(level);
    if (meter.value >= METER_CLIP || level >= METER_CLIP) onClip();

    barDbRef.current = advanceMeterBar(
      barDbRef.current,
      nextDb,
      now - (previousSampleAt ?? now),
    );
    setBarDb(barDbRef.current);

    if (Number.isFinite(nextDb) && nextDb >= heldDbRef.current) {
      heldDbRef.current = nextDb;
      holdUntilRef.current = now + holdDurationMs;
    } else if (now >= holdUntilRef.current) {
      const decayStart = Math.max(previousSampleAt ?? now, holdUntilRef.current);
      const elapsedSeconds = Math.max(0, now - decayStart) / 1000;
      const liveDb = Number.isFinite(nextDb) ? nextDb : -60;
      heldDbRef.current = Math.max(
        -60,
        liveDb,
        heldDbRef.current - elapsedSeconds * decayRateDbPerSecond,
      );
    }
    setHeldDb(heldDbRef.current);
  }, [meter?.sequence]);

  const cell = { gridColumn: column };
  return (
    <>
      <span className="dbfs-readout" style={cell}>
        {meter === null ? '—' : formatDbfs(readoutDb)}
      </span>
      <div className="dbfs-segments" style={cell} aria-hidden="true">
        <div className="dbfs-segment-track">
          <div
            className="dbfs-segment-fill"
            style={{ clipPath: `inset(${100 - litMeterSegments(displayedDb) * 100 / 60}% 0 0)` }}
          />
        </div>
        <b
          className="dbfs-peak-marker"
          style={{ bottom: `${dbfsPercent(heldDb)}%` }}
        />
      </div>
      {foot && <small className="dbfs-meter-foot" style={cell}>{foot}</small>}
    </>
  );
}

function DbScale({
  className,
  size,
}: {
  className: string;
  size: 'bridge' | 'strip' | 'master';
}) {
  return (
    <div className={`db-scale-meter db-scale-meter--${size} ${className}`} aria-hidden="true">
      <span className="db-scale-head-spacer" />
      <div className="db-scale">
        {[0, -3, -12, -24, -36, -48, -60].map((value) => (
          <span
            key={value}
            style={{ bottom: `${dbfsPercent(value)}%` }}
          >
            {value === 0 ? '0' : value}
          </span>
        ))}
      </div>
    </div>
  );
}

function ConsoleFader({
  label,
  value,
  min,
  max,
  step,
  onCommit,
  format,
  disabled = false,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onCommit: (value: number) => void;
  format: (value: number) => string;
  disabled?: boolean;
}) {
  const [draft, setDraft] = useState(value);
  const draftRef = useRef(value);
  useEffect(() => {
    draftRef.current = value;
    setDraft(value);
  }, [value]);

  const commit = () => {
    if (draftRef.current !== value) onCommit(draftRef.current);
  };
  const progress = ((draft - min) / (max - min)) * 100;
  return (
    <label className="console-fader">
      <span className="fader-rail">
        <span className="fader-scale-marks" aria-hidden="true">
          <span className="fader-scale-mark is-off" style={{ bottom: '0%' }}>
            <i />
            <b>−∞</b>
          </span>
          {[-48, -36, -24, -12, -6, 0].map((decibels) => (
            <span
              className="fader-scale-mark"
              key={decibels}
              style={{ bottom: `${10 ** (decibels / 40) * 100}%` }}
            >
              <i />
              <b>{decibels}</b>
            </span>
          ))}
        </span>
        <input
          type="range"
          min={min}
          max={max}
          step={step}
          value={draft}
          disabled={disabled}
          aria-label={label}
          aria-valuetext={format(draft)}
          style={{ '--fader-progress': `${progress}%` } as React.CSSProperties}
          onChange={(event) => {
            const next = Number(event.target.value);
            draftRef.current = next;
            setDraft(next);
          }}
          onPointerUp={commit}
          onKeyUp={commit}
          onBlur={commit}
        />
        <span className="fader-thumb-track" aria-hidden="true">
          <span className="fader-cap" style={{ bottom: `${progress}%` }}>
            {format(draft)}
          </span>
        </span>
      </span>
      <span className="fader-label">{label}</span>
    </label>
  );
}

function PanDial({
  value,
  label,
  onCommit,
  min = 0,
  max = 128,
  caption = 'PAN',
  format = formatPan,
  reset = 64,
  dimControl = false,
}: {
  value: number;
  label: string;
  onCommit: (value: number) => void;
  min?: number;
  max?: number;
  caption?: string;
  format?: (value: number) => string;
  reset?: number;
  dimControl?: boolean;
}) {
  const [draft, setDraft] = useState(value);
  const draftRef = useRef(value);
  const dragging = useRef(false);
  const dragStart = useRef<{ clientY: number; value: number } | null>(null);
  useEffect(() => {
    draftRef.current = value;
    setDraft(value);
  }, [value]);

  const setFromPointer = (event: React.PointerEvent<HTMLDivElement>) => {
    if (!dragStart.current) return;
    const delta = dragStart.current.clientY - event.clientY;
    const next = Math.max(
      min,
      Math.min(max, dragStart.current.value + Math.round(delta * 1.5)),
    );
    draftRef.current = next;
    setDraft(next);
  };
  const angle = -135 + (draft / 128) * 270;

  return (
    <div className={`pan-dial-control ${dimControl ? 'dim-dial-control' : ''}`}>
      <span className="pan-dial-label">{caption}</span>
      <div className="dial-face">
        <div
          className="pan-dial"
          role="slider"
          tabIndex={0}
          aria-label={label}
          aria-valuemin={min}
          aria-valuemax={max}
          aria-valuenow={draft}
          aria-valuetext={format(draft)}
          title={dimControl ? 'Nominal attenuation; integer DSP rounding can affect the actual level' : undefined}
          onPointerDown={(event) => {
            dragging.current = true;
            dragStart.current = { clientY: event.clientY, value: draftRef.current };
            event.currentTarget.setPointerCapture(event.pointerId);
            event.preventDefault();
            setFromPointer(event);
          }}
          onPointerMove={(event) => {
            if (dragging.current) setFromPointer(event);
          }}
          onPointerUp={(event) => {
            if (!dragging.current) return;
            setFromPointer(event);
            dragging.current = false;
            dragStart.current = null;
            onCommit(draftRef.current);
          }}
          onPointerCancel={() => {
            dragging.current = false;
            dragStart.current = null;
          }}
          onDoubleClick={() => {
            const next = Math.max(min, Math.min(max, reset));
            draftRef.current = next;
            setDraft(next);
            onCommit(next);
          }}
          onKeyDown={(event) => {
            let next = draft;
            if (event.key === 'ArrowRight' || event.key === 'ArrowUp') next += 1;
            else if (event.key === 'ArrowLeft' || event.key === 'ArrowDown') next -= 1;
            else if (event.key === 'Home') next = min;
            else if (event.key === 'End') next = max;
            else return;
            event.preventDefault();
            next = Math.max(min, Math.min(max, next));
            draftRef.current = next;
            setDraft(next);
            onCommit(next);
          }}
        >
          <i className="pan-dial-marker" style={{ transform: `rotate(${angle}deg)` }} />
        </div>
      </div>
      <span className="pan-dial-value">{format(draft)}</span>
    </div>
  );
}

function BusMasterStrip({
  status,
  meterStore,
  meterError,
  channelName,
  selectedBus,
  onControl,
}: {
  status: MixerStatus | null;
  meterStore: MeterStore;
  meterError: string;
  channelName: string;
  selectedBus: number;
  onControl: (
    control: ControlName,
    value: number,
    bus?: number,
    input?: number,
  ) => Promise<void>;
}) {
  return (
    <aside className="bus-master-panel panel">
      <header className="bus-master-heading">
        <span className="eyebrow">BUS MASTER · {String(selectedBus + 1).padStart(2, '0')}</span>
        <strong>
          {status
            ? displayChannelName(status.bus.label, channelName)
            : 'Loading bus…'}
        </strong>
        {status?.bus.available === false && <small className="availability-pill">NOT PRESENT</small>}
      </header>

      <div className="bus-master-controls">
        <DbScale className="bus-master-scale" size="master" />
        <MeterGroup
          key={`host-${selectedBus}`}
          title="HOST"
          meterStore={meterStore}
          indices={[0, 1]}
          kind="output"
          bus={selectedBus}
          labels={['Host left', 'Host right']}
          feet={['L', 'R']}
          size="master"
        />
        <div className="bus-master-fader">
          <ConsoleFader
            label="BUS"
            value={status?.bus.volume ?? 0}
            min={0}
            max={32768}
            step={128}
            format={formatGain}
            disabled={!status}
            onCommit={(value) => void onControl('bus.volume', value, selectedBus)}
          />
          <EngagedButton
            label="Mute"
            tone="mute"
            engaged={status ? !status.bus.enabled : false}
            disabled={!status}
            ariaLabel={`Mute ${status ? displayChannelName(status.bus.label, channelName) : 'selected bus'}`}
            onChange={(muted) => void onControl('bus.enabled', Number(!muted), selectedBus)}
          />
        </div>
        <MeterGroup
          key={`mix-${selectedBus}`}
          title="MIX"
          meterStore={meterStore}
          indices={[2, 3]}
          kind="output"
          bus={selectedBus}
          labels={['Mix left', 'Mix right']}
          feet={['L', 'R']}
          size="master"
        />
        <DbScale className="bus-master-scale" size="master" />
      </div>
      {meterError && <p className="meter-error" role="status">{meterError}</p>}
    </aside>
  );
}

function TalkbackView({
  status,
  channelNames,
  onControl,
}: {
  status: MixerStatus | null;
  channelNames: ChannelNames;
  onControl: (
    control: ControlName,
    value: number,
    bus?: number,
    input?: number,
  ) => Promise<void>;
}) {
  if (!status) {
    return <main className="single-stage"><div className="loading-panel panel"><div className="spinner" /><span>Reading mixer…</span></div></main>;
  }
  return (
    <main className="single-stage talkback-stage">
      <div className="page-heading">
        <div>
          <p className="eyebrow">COMMUNICATION <span className="slash">/</span> ROUTING</p>
          <h1>Talkback &amp; listenback</h1>
        </div>
      </div>
      <div className="feature-grid">
        <FeatureCard
          name="Talkback"
          value={status.talkback}
          channelNames={channelNames}
          onControl={onControl}
        />
        <FeatureCard
          name="Listenback"
          value={status.listenback}
          channelNames={channelNames}
          onControl={onControl}
        />
      </div>
    </main>
  );
}

function FeatureCard({
  name,
  value,
  channelNames,
  onControl,
}: {
  name: 'Talkback' | 'Listenback';
  value: Feature;
  channelNames: ChannelNames;
  onControl: (
    control: ControlName,
    value: number,
    bus?: number,
    input?: number,
  ) => Promise<void>;
}) {
  const key = name.toLowerCase() as 'talkback' | 'listenback';
  return (
    <section className={`feature-card panel ${value.enabled ? 'feature-live' : ''}`}>
      <div className="feature-head">
        <div>
          <span className="eyebrow">MICROPHONE ROUTING</span>
          <h2>{name} <PowerButton
            active={value.enabled}
            ariaLabel={name}
            onChange={(enabled) => void onControl(`${key}.enabled`, Number(enabled))}
          /></h2>
        </div>
      </div>
      <SourceSelector
        name={name}
        value={value.input}
        channelNames={channelNames.inputs}
        onChange={(input) => void onControl(`${key}.input`, input)}
      />
      <PanDial
        label={`${name} dim other routes`}
        caption="DIM"
        value={value.dim}
        reset={128}
        format={formatDim}
        dimControl
        onCommit={(next) => void onControl(`${key}.dim`, next)}
      />
      <div className="destination-heading">
        <div>
          <span className="eyebrow">DESTINATIONS</span>
        </div>
        <span className="selection-count">{value.buses.filter(Boolean).length} / 48</span>
      </div>
      <div className="destination-grid">
        {value.buses.map((enabled, bus) => {
          const identifier = `Bus ${String(bus + 1).padStart(2, '0')}`;
          const label = displayChannelName(identifier, channelNames.outputs[bus]);
          return (
            <button
              key={bus}
              className={`destination-chip ${enabled ? 'is-on' : ''}`}
              onClick={() => void onControl(`${key}.bus`, Number(!enabled), bus)}
              title={label}
              aria-label={`${label} destination`}
              aria-pressed={enabled}
            >
              <span>{String(bus + 1).padStart(2, '0')}</span>
              <i />
            </button>
          );
        })}
      </div>
    </section>
  );
}

function SourceSelector({
  name,
  value,
  channelNames,
  onChange,
}: {
  name: string;
  value: number;
  channelNames: string[];
  onChange: (value: number) => void;
}) {
  const [open, setOpen] = useState(false);
  const [focused, setFocused] = useState(value);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const listRef = useRef<HTMLDivElement>(null);
  const searchRef = useRef({ text: '', time: 0 });
  const labels = Array.from({ length: 96 }, (_, index) =>
    displayChannelName(`Input ${String(index + 1).padStart(2, '0')}`, channelNames[index]));
  labels.push('No input');

  useEffect(() => {
    if (open) listRef.current?.querySelector<HTMLButtonElement>(`[data-input="${focused}"]`)?.focus();
  }, [open, focused]);

  const close = () => {
    setOpen(false);
    triggerRef.current?.focus();
  };
  return (
    <div
      className="select-field source-selector"
      onBlur={(event) => {
        if (!event.currentTarget.contains(event.relatedTarget)) setOpen(false);
      }}
    >
      <span>Source input</span>
      <button
        type="button"
        ref={triggerRef}
        className="source-selector-trigger"
        aria-label={`${name} source input`}
        aria-haspopup="listbox"
        aria-expanded={open}
        onClick={() => {
          setFocused(value);
          setOpen(!open);
        }}
        onKeyDown={(event) => {
          if (!open && (event.key === 'ArrowDown' || event.key === 'ArrowUp')) {
            event.preventDefault();
            setFocused(value);
            setOpen(true);
          }
        }}
      >
        <span>{labels[value]}</span><span aria-hidden="true">▾</span>
      </button>
      {open && (
        <div
          ref={listRef}
          className="source-selector-list"
          role="listbox"
          aria-label={`${name} source input`}
          onKeyDown={(event) => {
            let next = focused;
            if (event.key === 'ArrowDown') next = Math.min(96, focused + 1);
            else if (event.key === 'ArrowUp') next = Math.max(0, focused - 1);
            else if (event.key === 'Home') next = 0;
            else if (event.key === 'End') next = 96;
            else if (event.key === 'Escape') {
              event.preventDefault();
              close();
              return;
            } else if (event.key.length === 1 && event.key !== ' ' && !event.ctrlKey && !event.metaKey && !event.altKey) {
              const now = performance.now();
              const previous = now - searchRef.current.time < 700 ? searchRef.current.text : '';
              const text = `${previous}${event.key}`.toLowerCase();
              searchRef.current = { text, time: now };
              const found = labels.findIndex((label) => label.toLowerCase().startsWith(text));
              if (found >= 0) next = found;
            } else return;
            event.preventDefault();
            setFocused(next);
          }}
        >
          {labels.map((label, index) => (
            <button
              type="button"
              role="option"
              key={index}
              data-input={index}
              tabIndex={index === focused ? 0 : -1}
              aria-selected={index === value}
              onFocus={() => setFocused(index)}
              onClick={() => {
                if (index !== value) onChange(index);
                close();
              }}
            >{label}</button>
          ))}
        </div>
      )}
    </div>
  );
}

function deviceSettingGroup(name: string): string {
  if (name.startsWith('SMPTE Generator ')) return 'SMPTE generator';
  if (name.startsWith('SMPTE Reader ') || name.startsWith('SMPTE Freewheel ')) return 'SMPTE reader';
  if (name.startsWith('SMPTE ')) return 'SMPTE format';
  if (name.includes('Clock Source') || name === 'Interface Word Clock Out Rate') return 'Clock';
  if (name === 'Card S/MUX Type') return 'Digital audio';
  if (name.startsWith('Interface 2408 ') && name.includes('Bank')) return 'Banks';
  if (name.includes('-10dBV Switch')) return 'Input reference levels';
  if (name.startsWith('Interface AES ')) return 'AES / sample-rate conversion';
  if (name === 'Interface Peak Hold Time' || name === 'Interface Clip Hold Time') return 'Metering';
  return 'Other settings';
}

function deviceSettingGroups(settings: DeviceSetting[]): { title: string; settings: DeviceSetting[] }[] {
  const order = [
    'Clock', 'Digital audio', 'Banks', 'Input reference levels',
    'AES / sample-rate conversion', 'Metering', 'SMPTE format',
    'SMPTE reader', 'SMPTE generator', 'Other settings',
  ];
  const groups = new Map<string, DeviceSetting[]>();
  for (const setting of settings) {
    const title = deviceSettingGroup(setting.name);
    const group = groups.get(title);
    if (group) group.push(setting);
    else groups.set(title, [setting]);
  }
  return order.flatMap((title) => {
    const group = groups.get(title);
    return group ? [{ title, settings: group }] : [];
  });
}

function DevicesView({
  csrf,
  onError,
}: {
  csrf: string | null;
  onError: (message: string) => void;
}) {
  const [devices, setDevices] = useState<HardwareDevice[] | null>(null);
  const [loading, setLoading] = useState(true);
  const [pending, setPending] = useState(false);

  useEffect(() => {
    if (pending) return;
    let active = true;
    const refresh = async () => {
      try {
        const result = await api<{ devices: HardwareDevice[] }>('/api/devices');
        if (active) setDevices(result.devices);
      } catch (cause) {
        if (active) onError((cause as Error).message);
      } finally {
        if (active) setLoading(false);
      }
    };
    void refresh();
    const timer = window.setInterval(refresh, 3000);
    return () => {
      active = false;
      window.clearInterval(timer);
    };
  }, [pending, onError]);

  const change = async (setting: DeviceSetting, values: number[]) => {
    setPending(true);
    onError('');
    try {
      const result = await api<{ devices: HardwareDevice[] }>(
        '/api/devices/control',
        { method: 'POST', body: JSON.stringify({ name: setting.name, index: setting.index, values }) },
        csrf,
      );
      setDevices(result.devices);
    } catch (cause) {
      onError((cause as Error).message);
    } finally {
      setPending(false);
    }
  };

  return (
    <main className="single-stage devices-stage">
      <div className="page-heading"><h1>Devices</h1></div>
      {loading && <div className="loading-panel panel"><div className="spinner" /><span>Reading devices…</span></div>}
      {!loading && !devices && <p role="status">Device settings unavailable.</p>}
      {devices?.map((device) => (
        <section className="device-panel panel" key={device.port ?? 'card'}>
          <header className="section-heading">
            <h2>{device.name}</h2>
            <span className="eyebrow">
              {device.port === null ? 'PCI(e) CARD' : `AUDIOWIRE ${device.port + 1}`}
              {device.configured === false ? ' · NOT CONFIGURED' : ''}
            </span>
          </header>
          {device.controls.length === 0 && (
            <p className="device-empty">
              {device.configured === false ? 'Waiting for device configuration.' : 'No settings published by this driver.'}
            </p>
          )}
          <div className="device-groups">
            {deviceSettingGroups(device.controls).map((group) => (
              <section className="device-group" key={group.title}>
                <h3>{group.title}</h3>
                {device.port === null && group.title === 'Clock' && (
                  <div className="clock-detection" role="status">
                    <span className="clock-detection-dot" aria-hidden="true" />
                    <span title="The driver does not report external clock lock or rate">
                      External clock detection <strong>Unavailable</strong>
                    </span>
                  </div>
                )}
                {device.port === null && group.title === 'Clock' && (
                  <p className="device-group-warning">Changing clock source can interrupt audio.</p>
                )}
                {group.title === 'SMPTE generator' && (
                  <p className="device-group-warning">Generating SMPTE can replace audio on its destination.</p>
                )}
                <div className="device-settings">
                  {group.settings.map((setting) => (
                    <DeviceSettingEditor
                      key={`${setting.name}:${setting.index}`}
                      setting={setting}
                      disabled={pending || device.configured === false}
                      onChange={(values) => void change(setting, values)}
                    />
                  ))}
                </div>
              </section>
            ))}
          </div>
        </section>
      ))}
    </main>
  );
}

function DeviceSettingEditor({
  setting,
  disabled,
  onChange,
}: {
  setting: DeviceSetting;
  disabled: boolean;
  onChange: (values: number[]) => void;
}) {
  const [draft, setDraft] = useState(setting.values);
  const signature = setting.values.join(',');
  useEffect(() => {
    setDraft(signature.split(',').map(Number));
  }, [signature]);
  const id = `device-${setting.index}-${setting.name.replaceAll(' ', '-')}`;
  const valueLabel = (value: number) =>
    setting.options.find((option) => option.value === value)?.label
      ?? (setting.kind === 'boolean' ? (value ? 'On' : 'Off') : String(value));
  return (
    <form
      className="device-setting"
      onSubmit={(event) => {
        event.preventDefault();
        onChange(draft);
      }}
    >
      <label htmlFor={id} title={setting.name}>{setting.label}</label>
      {!setting.writable ? (
        <output id={id}>{setting.values.map(valueLabel).join(' : ')}</output>
      ) : setting.clear_only ? (
        <button
          id={id}
          type="button"
          className="button button-quiet"
          disabled={disabled || !setting.values[0]}
          onClick={() => onChange([0])}
        >{setting.values[0] ? 'Detected · Clear' : 'Clear'}</button>
      ) : setting.options.length > 0 ? (
        <select
          id={id}
          value={setting.values[0]}
          disabled={disabled}
          onChange={(event) => onChange([Number(event.target.value)])}
        >
          {!setting.options.some((option) => option.value === setting.values[0]) && (
            <option value={setting.values[0]}>Code {setting.values[0]} (unavailable)</option>
          )}
          {setting.options.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}
        </select>
      ) : setting.kind === 'boolean' ? (
        <Toggle
          label={setting.values[0] ? 'On' : 'Off'}
          active={!!setting.values[0]}
          disabled={disabled}
          ariaLabel={setting.label}
          onChange={(value) => onChange([Number(value)])}
        />
      ) : (
        <div className="device-value-editor">
          {draft.map((value, index) => (
            <input
              key={index}
              id={index === 0 ? id : `${id}-${index}`}
              type="number"
              required
              min={setting.min}
              max={setting.max}
              step={setting.step}
              value={Number.isNaN(value) ? '' : value}
              disabled={disabled}
              aria-label={`${setting.label}${draft.length > 1 ? ` · ${['hours', 'minutes', 'seconds', 'frames'][index] ?? index + 1}` : ''}`}
              onChange={(event) => {
                const next = event.target.valueAsNumber;
                setDraft((current) => current.map((member, position) => position === index ? next : member));
              }}
            />
          ))}
          <button
            className="button button-quiet"
            disabled={disabled || draft.join(',') === signature}
          >Set</button>
        </div>
      )}
      {setting.note && <small>{setting.note}</small>}
    </form>
  );
}

function ChannelNamesView({
  names,
  status,
  busy,
  onSave,
}: {
  names: ChannelNames;
  status: MixerStatus | null;
  busy: boolean;
  onSave: (names: ChannelNames) => Promise<void>;
}) {
  const [kind, setKind] = useState<'inputs' | 'outputs'>('inputs');
  const [draft, setDraft] = useState(names);
  const [search, setSearch] = useState('');
  useEffect(() => setDraft(names), [names]);

  const changed = draft.inputs.some((name, index) => name !== names.inputs[index])
    || draft.outputs.some((name, index) => name !== names.outputs[index]);
  const count = kind === 'inputs' ? 96 : 48;
  const query = search.trim().toLowerCase();
  const visibleIndices = Array.from({ length: count }, (_, index) => index).filter((index) => {
    const number = String(index + 1).padStart(2, '0');
    const identifier = kind === 'inputs' ? `input ${number}` : `bus ${number}`;
    const detail = kind === 'inputs' ? status?.inputs[index]?.label ?? '' : 'stereo output pair';
    return (
      !query
      || identifier.includes(query)
      || detail.toLowerCase().includes(query)
      || draft[kind][index].toLowerCase().includes(query)
    );
  });

  const updateName = (index: number, value: string) => {
    setDraft((current) => kind === 'inputs'
      ? {
          ...current,
          inputs: current.inputs.map((name, currentIndex) =>
            currentIndex === index ? value : name),
        }
      : {
          ...current,
          outputs: current.outputs.map((name, currentIndex) =>
            currentIndex === index ? value : name),
        });
  };

  return (
    <main className="single-stage channel-names-stage">
      <div className="page-heading channel-names-heading">
        <div>
          <p className="eyebrow">MIXER SETUP <span className="slash">/</span> LABELS</p>
          <h1>Channel names</h1>
        </div>
        <div className="channel-name-actions">
          {changed && <span className="channel-name-unsaved">Unsaved changes</span>}
          <button
            className="button button-quiet"
            disabled={!changed || busy}
            onClick={() => setDraft(names)}
          >
            Discard
          </button>
          <button
            className="button button-primary"
            disabled={!changed || busy}
            onClick={() => void onSave(draft)}
          >
            {busy ? 'Saving…' : 'Save names'}
          </button>
        </div>
      </div>

      <section className="channel-names-panel panel">
        <div className="channel-names-toolbar">
          <div className="channel-name-tabs" role="tablist" aria-label="Channel type">
            <button
              className={kind === 'inputs' ? 'is-selected' : ''}
              role="tab"
              aria-selected={kind === 'inputs'}
              onClick={() => setKind('inputs')}
            >
              Inputs <span>96</span>
            </button>
            <button
              className={kind === 'outputs' ? 'is-selected' : ''}
              role="tab"
              aria-selected={kind === 'outputs'}
              onClick={() => setKind('outputs')}
            >
              Output pairs <span>48</span>
            </button>
          </div>
          <label className="search-field channel-name-search">
            <SearchIcon />
            <input
              aria-label="Filter channels"
              value={search}
              onChange={(event) => setSearch(event.target.value)}
              placeholder="Find a channel"
            />
          </label>
        </div>

        <div className="channel-name-list">
          {visibleIndices.map((index) => {
            const number = String(index + 1).padStart(2, '0');
            const identifier = kind === 'inputs' ? `INPUT ${number}` : `BUS PAIR ${number}`;
            const detail = kind === 'inputs'
              ? status?.inputs[index]?.label ?? `Physical input ${number}`
              : 'Stereo output pair';
            return (
              <label className="channel-name-row" key={`${kind}-${index}`}>
                <span className="channel-name-identifier">
                  <strong>{identifier}</strong>
                  <small>{detail}</small>
                </span>
                <input
                  type="text"
                  maxLength={48}
                  aria-label={`${identifier} custom name`}
                  value={draft[kind][index]}
                  placeholder="No custom name"
                  onChange={(event) => updateName(index, event.target.value)}
                />
              </label>
            );
          })}
          {visibleIndices.length === 0 && (
            <div className="channel-name-empty">
              No matching channels. Try a different search.
            </div>
          )}
        </div>
      </section>
    </main>
  );
}

function ProfilesView({
  profiles,
  selected,
  status,
  channelNames,
  selectedBus,
  busy,
  onSelect,
  onCreate,
  onApply,
  onDelete,
  onReplace,
}: {
  profiles: ProfileSummary[];
  selected: ProfileDetail | null;
  status: MixerStatus | null;
  channelNames: ChannelNames;
  selectedBus: number;
  busy: boolean;
  onSelect: (id: string) => void;
  onCreate: (name: string) => void;
  onApply: () => void;
  onDelete: (id: string) => void;
  onReplace: (id: string) => Promise<void>;
}) {
  const [name, setName] = useState('');
  const [copying, setCopying] = useState(false);
  const currentBus = selected?.snapshot.buses[selectedBus];
  const routeCount = currentBus?.routes.filter((route) => route.enabled).length ?? 0;
  const targetBusEnabled = currentBus?.enabled ?? false;
  const routeChanges = currentBus && status
    ? currentBus.routes.flatMap((target, input) => {
        const current = status.inputs[input].route;
        return (
          target.volume !== current.volume
          || target.pan !== current.pan
          || target.enabled !== current.enabled
          || target.solo !== current.solo
        )
          ? [{ input, current, target }]
          : [];
      })
    : [];
  return (
    <main className="single-stage profiles-stage">
      <div className="page-heading">
        <div>
          <p className="eyebrow">SCENES <span className="slash">/</span> RECALL</p>
          <h1>Mix profiles</h1>
        </div>
        <div className="profile-save">
          <input
            aria-label="New profile name"
            value={name}
            onChange={(event) => setName(event.target.value)}
            placeholder="Name this mix"
            maxLength={48}
          />
          <button
            className="button button-primary"
            disabled={busy || !name.trim()}
            onClick={() => {
              onCreate(name.trim());
              setName('');
            }}
          >
            <PlusIcon /> Save current mix
          </button>
        </div>
      </div>
      <div className="profile-layout">
        <section className="profile-list panel">
          <div className="panel-title-row">
            <div>
              <span className="eyebrow">SAVED LOCALLY</span>
              <h2>Your profiles <span className="subtle-count">{profiles.length}</span></h2>
            </div>
          </div>
          {profiles.length === 0 ? (
            <div className="empty-state compact-empty">
              <WaveIcon />
              <strong>No profiles yet</strong>
              <span>Save the current mix to create your first scene.</span>
            </div>
          ) : profiles.map((profile) => (
            <div className={`profile-list-item ${selected?.id === profile.id ? 'is-selected' : ''}`} key={profile.id}>
              <button className="profile-select" onClick={() => onSelect(profile.id)}>
                <span className="profile-glyph"><WaveIcon /></span>
                <span className="profile-list-copy">
                  <strong>{profile.name}</strong>
                  <small>{new Date(profile.updated * 1000).toLocaleDateString()}</small>
                </span>
                <ChevronIcon />
              </button>
              <button className="icon-button delete-profile" onClick={() => onDelete(profile.id)} aria-label={`Delete ${profile.name}`}>
                <TrashIcon />
              </button>
            </div>
          ))}
          <p className="profile-storage-note"><LockIcon /> Profiles stay on this mixer host.</p>
        </section>

        <section className="profile-preview panel">
          {!selected ? (
            <div className="profile-empty-preview">
              <div className="preview-orbit"><WaveIcon /></div>
              <span className="eyebrow">PROFILE PREVIEW</span>
              <h2>Select a saved mix</h2>
              <p>Review its settings before applying changes to the live mixer.</p>
            </div>
          ) : (
            <>
              <div className="preview-head">
                <div>
                  <span className="eyebrow">READY TO PREVIEW</span>
                  <h2>{selected.name}</h2>
                </div>
                <span className="change-count">{selected.changed} <small>values differ</small></span>
              </div>
              <div className="preview-bus-card">
                <div className="preview-bus-heading">
                  <div>
                    <span className="eyebrow">SELECTED BUS</span>
                    <strong>
                      {displayChannelName(
                        status?.bus.label ?? `Bus ${selectedBus + 1}`,
                        channelNames.outputs[selectedBus],
                      )}
                    </strong>
                  </div>
                  <span className={`state-pill ${targetBusEnabled ? 'is-on' : ''}`}>
                    {targetBusEnabled ? 'ON' : 'MUTED'}
                  </span>
                </div>
                <div className="preview-stat-row">
                  <div>
                    <span>Enabled routes</span>
                    <strong>{routeCount}</strong>
                  </div>
                  <div>
                    <span>Bus level</span>
                    <strong>{formatGain(currentBus?.volume ?? 0)}</strong>
                  </div>
                  <div>
                    <span>Talkback</span>
                    <strong>{selected.snapshot.talkback.enabled ? 'ON' : 'OFF'}</strong>
                  </div>
                  <div>
                    <span>Listenback</span>
                    <strong>{selected.snapshot.listenback.enabled ? 'ON' : 'OFF'}</strong>
                  </div>
                  <div className="route-change-list">
                    <span className="eyebrow">ROUTE CHANGES ON THIS BUS</span>
                    {routeChanges.length === 0 ? (
                      <p>No route changes for the selected bus.</p>
                    ) : (
                      <>
                        {routeChanges.slice(0, 5).map(({ input, current, target }) => (
                          <div className="route-change" key={input}>
                            <span>
                              {displayChannelName(
                                `IN ${String(input + 1).padStart(2, '0')}`,
                                channelNames.inputs[input],
                              )}
                            </span>
                            <span>{current.enabled ? 'ON' : 'OFF'} <i>→</i> {target.enabled ? 'ON' : 'OFF'}</span>
                            <span>{formatGain(current.volume)} <i>→</i> {formatGain(target.volume)}</span>
                          </div>
                        ))}
                        {routeChanges.length > 5 && (
                          <p>+ {routeChanges.length - 5} more route changes</p>
                        )}
                      </>
                    )}
                  </div>
                </div>
              </div>
              <div className="preview-warning">
                <InfoIcon />
                <span>Applying a profile briefly mutes buses while settings are written. The live mix may change audibly.</span>
              </div>
              <button className="button button-primary apply-button" disabled={busy} onClick={onApply}>
                <CheckIcon /> Apply to live mixer
              </button>
              <button
                className="text-button update-profile"
                disabled={busy || copying}
                onClick={async () => {
                  if (!window.confirm(`Replace “${selected.name}” with the current live mix?`)) return;
                  setCopying(true);
                  try {
                    await onReplace(selected.id);
                  } finally {
                    setCopying(false);
                  }
                }}
              >
                {copying ? 'Updating…' : 'Replace with current mix'}
              </button>
            </>
          )}
        </section>
      </div>
    </main>
  );
}

function Toggle({
  label,
  active,
  onChange,
  className = '',
  ariaLabel,
  disabled = false,
}: {
  label: string;
  active: boolean;
  onChange: (active: boolean) => void;
  className?: string;
  ariaLabel?: string;
  disabled?: boolean;
}) {
  return (
    <button
      type="button"
      className={`toggle-control ${active ? 'is-active' : ''} ${className}`}
      disabled={disabled}
      onClick={() => onChange(!active)}
      aria-pressed={active}
      aria-label={ariaLabel}
    >
      <span className="toggle-track"><i /></span>
      <span>{label}</span>
    </button>
  );
}

function PowerButton({
  active,
  ariaLabel,
  disabled = false,
  onChange,
}: {
  active: boolean;
  ariaLabel: string;
  disabled?: boolean;
  onChange: (active: boolean) => void;
}) {
  return (
    <button
      type="button"
      className={`power-button ${active ? 'is-on' : ''}`}
      disabled={disabled}
      onClick={() => onChange(!active)}
      aria-label={ariaLabel}
      aria-pressed={active}
      title={`${ariaLabel}: ${active ? 'on' : 'off'}`}
    >
      <svg viewBox="0 0 24 24" aria-hidden="true">
        <path d="M12 3v9" />
        <path d="M6.6 6.8a8 8 0 1 0 10.8 0" />
      </svg>
    </button>
  );
}

function EngagedButton({
  label,
  tone,
  engaged,
  ariaLabel,
  disabled = false,
  onChange,
}: {
  label: string;
  tone: 'mute' | 'solo' | 'hide';
  engaged: boolean;
  ariaLabel: string;
  disabled?: boolean;
  onChange: (engaged: boolean) => void;
}) {
  return (
    <button
      type="button"
      className={`engaged-button engaged-button--${tone} ${engaged ? 'is-engaged' : ''}`}
      disabled={disabled}
      onClick={() => onChange(!engaged)}
      aria-label={ariaLabel}
      aria-pressed={engaged}
    >
      {label}
    </button>
  );
}

function ControlSlider({
  label,
  value,
  min,
  max,
  step,
  onCommit,
  format = (current) => `${current}`,
  center,
  compact = false,
  disabled = false,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onCommit: (value: number) => void;
  format?: (value: number) => string;
  center?: number;
  compact?: boolean;
  disabled?: boolean;
}) {
  const [draft, setDraft] = useState(value);
  const draftRef = useRef(value);
  useEffect(() => {
    draftRef.current = value;
    setDraft(value);
  }, [value]);
  const commit = () => {
    if (draftRef.current !== value) onCommit(draftRef.current);
  };
  const progress = max === min ? 50 : ((draft - min) / (max - min)) * 100;
  return (
    <label className={`slider-control ${compact ? 'is-compact' : ''} ${disabled ? 'is-disabled' : ''}`}>
      <span className="slider-label">{label}</span>
      <span className="slider-rail">
        <input
          type="range"
          min={min}
          max={max}
          step={step}
          value={draft}
          disabled={disabled || min === max}
          style={{ '--range-progress': `${progress}%` } as React.CSSProperties}
          onChange={(event) => {
            const next = Number(event.target.value);
            draftRef.current = next;
            setDraft(next);
          }}
          onPointerUp={commit}
          onKeyUp={commit}
          onBlur={commit}
        />
        {center !== undefined && max !== min && <span className="slider-center" style={{ left: `${((center - min) / (max - min)) * 100}%` }} />}
      </span>
      <span className="slider-value">{format(draft)}</span>
    </label>
  );
}

function peakToDbfs(level: number): number {
  return level <= 0 ? -Infinity : Math.min(0, 20 * Math.log10(level / METER_FULL_SCALE));
}

// Input peaks are measured before the trim stage and the input switch.
function inputMeterGain(input: InputChannel | undefined): number {
  if (!input) return 1;
  return input.enabled ? input.trim / TRIM_UNITY : 0;
}

function dbfsPercent(value: number): number {
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(100, ((value + 60) / 60) * 100));
}

function formatDbfs(value: number): string {
  return Number.isFinite(value) ? `${Math.round(value)}` : '−∞';
}

function displayChannelName(identifier: string, customName: string): string {
  const name = customName.trim();
  return name ? `${name} · ${identifier}` : identifier;
}

function formatGain(value: number) {
  if (value <= 0) return '−∞ dB';
  return `${(40 * Math.log10(value / 32768)).toFixed(1)} dB`;
}

function formatDim(value: number) {
  if (value <= 0) return '−∞ dB';
  return `${(40 * Math.log10(value / 128)).toFixed(1)} dB`;
}

function formatTrim(value: number) {
  const decibels = 20 * Math.log10(value / 64);
  return decibels === 0 ? '0.0 dB' : `+${decibels.toFixed(1)} dB`;
}

function formatPan(value: number) {
  if (value === 64) return 'C';
  return value < 64 ? `L ${64 - value}` : `R ${value - 64}`;
}

function formatWidth(value: number) {
  if (value === 128) return 'STEREO';
  if (value === 64) return 'MONO';
  if (value === 0) return 'REVERSED';
  return `${Math.round(((value - 64) / 64) * 100)}%`;
}

function isMeterFrame(value: object): value is MeterFrame {
  const frame = value as Partial<MeterFrame>;
  return (
    (frame.input_bank === 0 || frame.input_bank === 1)
    && Number.isInteger(frame.bus)
    && Array.isArray(frame.values)
    && frame.values.length === 52
    && frame.values.every((peak) => Number.isInteger(peak) && peak >= 0 && peak <= 65535)
    && Number.isInteger(frame.sequence)
    && typeof frame.sampled_at === 'number'
  );
}

function isFlagList<Key extends string>(
  value: unknown,
  key: Key,
  count: number,
): value is Record<Key, boolean[]> {
  if (typeof value !== 'object' || value === null || !(key in value)) return false;
  const flags = (value as Record<Key, unknown>)[key];
  return Array.isArray(flags) && flags.length === count && flags.every(
    (enabled) => typeof enabled === 'boolean',
  );
}

function sameFlags(left: boolean[], right: boolean[]): boolean {
  return left.every((value, index) => value === right[index]);
}

function mergeInputs(
  current: MixerStatus,
  updates: InputUpdate[],
  bus?: number,
): MixerStatus {
  const byIndex = new Map(updates.map((update) => [update.index, update]));
  return {
    ...current,
    inputs: current.inputs.map((channel) => {
      const update = byIndex.get(channel.index);
      if (!update) return channel;
      const next = { ...channel, trim: update.trim, enabled: update.enabled };
      if (update.route && bus === current.bus.index) {
        next.route = update.route;
        next.stereo = update.stereo ?? null;
      }
      return next;
    }),
  };
}

function patchStatus(
  current: MixerStatus,
  control: ControlName,
  value: number,
  bus?: number,
  input?: number,
): MixerStatus {
  const next = { ...current };
  if (control.startsWith('input.') && input !== undefined) {
    next.inputs = current.inputs.map((channel) =>
      channel.index !== input
        ? channel
        : {
            ...channel,
            ...(control === 'input.trim' ? { trim: value } : { enabled: Boolean(value) }),
          },
    );
  } else if (control.startsWith('route.') && input !== undefined) {
    next.inputs = current.inputs.map((channel) =>
      channel.index !== input
        ? channel
        : {
            ...channel,
            route: {
              ...channel.route,
              [control.split('.')[1]]: control === 'route.volume' || control === 'route.pan'
                ? value
                : Boolean(value),
            },
          },
    );
  } else if (control.startsWith('bus.') && bus === current.bus.index) {
    next.bus = {
      ...current.bus,
      ...(control === 'bus.volume' ? { volume: value } : { enabled: Boolean(value) }),
    };
  } else if (control.startsWith('talkback.') || control.startsWith('listenback.')) {
    const featureKey = control.startsWith('talkback.') ? 'talkback' : 'listenback';
    const field = control.split('.')[1];
    if (field === 'bus' && bus !== undefined) {
      next[featureKey] = {
        ...current[featureKey],
        buses: current[featureKey].buses.map((enabled, index) => index === bus ? Boolean(value) : enabled),
      };
    } else {
      next[featureKey] = {
        ...current[featureKey],
        [field]: field === 'enabled' ? Boolean(value) : value,
      };
    }
  } else if (control === 'meter.bus') {
    next.meter = {
      ...current.meter,
      bus: value,
    };
  }
  return next;
}

function SearchIcon() {
  return <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="10.8" cy="10.8" r="6.8" /><path d="m16 16 4.2 4.2" /></svg>;
}

function WaveIcon() {
  return <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 12h2l2-6 4 12 3-9 2 6 2-3h3" /></svg>;
}

function ArrowIcon() {
  return <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M5 12h14M13 6l6 6-6 6" /></svg>;
}

function ChevronIcon({ down = false }: { down?: boolean }) {
  return <svg className={down ? 'chevron-down' : ''} viewBox="0 0 24 24" aria-hidden="true"><path d="m9 18 6-6-6-6" /></svg>;
}

function LogoutIcon() {
  return <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M10 17l5-5-5-5M15 12H3M12 3h6a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-6" /></svg>;
}

function LockIcon() {
  return <svg viewBox="0 0 24 24" aria-hidden="true"><rect x="4" y="10" width="16" height="11" rx="2" /><path d="M8 10V7a4 4 0 1 1 8 0v3" /></svg>;
}

function InfoIcon() {
  return <svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9" /><path d="M12 11v5M12 8h.01" /></svg>;
}

function PlusIcon() {
  return <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg>;
}

function TrashIcon() {
  return <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 7h16M10 11v6M14 11v6M5 7l1 14h12l1-14M9 7V4h6v3" /></svg>;
}

function CheckIcon() {
  return <svg viewBox="0 0 24 24" aria-hidden="true"><path d="m5 12 4 4L19 6" /></svg>;
}

export default App;
