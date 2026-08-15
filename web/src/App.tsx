import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import { ZMKConnection } from "@cormoran/zmk-studio-react-hook";
import { SensorInfo } from "./SensorInfo";
import { SettingsPanel } from "./SettingsPanel";
import { useStudioLockState } from "./useStudioLockState";

function ConnectedContent({ deviceName }: { deviceName: string | undefined }) {
  const { locked } = useStudioLockState();

  return (
    <>
      <section className="card">
        <h2>Device Connection</h2>
        <div className="device-info">
          <h3>Connected to: {deviceName}</h3>
        </div>
      </section>

      {locked && (
        <div className="locked-banner" role="alert">
          ZMK Studio is locked — press the Studio unlock key on your keyboard to
          use this module. Sensor settings and diagnostics are disabled until
          unlocked.
        </div>
      )}

      <SensorInfo />
      <SettingsPanel locked={locked} />
    </>
  );
}

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>PAW3222 Trackball</h1>
        <p>Sensor configurator (custom Studio RPC)</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>{error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <ConnectedContent deviceName={deviceName} />
            <section className="card">
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>PAW3222 Module</strong> - sensor settings and diagnostics over
          custom Studio RPC
        </p>
      </footer>
    </div>
  );
}

export default App;
