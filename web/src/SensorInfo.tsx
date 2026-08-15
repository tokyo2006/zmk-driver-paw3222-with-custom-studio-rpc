import { useContext, useEffect, useState } from "react";
import { ZMKCustomSubsystem, ZMKAppContext } from "@cormoran/zmk-studio-react-hook";
import {
  DeviceInfo,
  ReadDiagnosticsResponse,
  Request,
  Response,
} from "./proto/xinta/paw3222/paw3222";

export const PAW3222_SUBSYSTEM_IDENTIFIER = "xinta__paw3222";
export const PAW3222_PRODUCT_ID = 0x30;

export function SensorInfo() {
  const zmkApp = useContext(ZMKAppContext);
  const subsystem = zmkApp?.findSubsystem(PAW3222_SUBSYSTEM_IDENTIFIER);

  const [devices, setDevices] = useState<DeviceInfo[]>([]);
  const [selectedDevice, setSelectedDevice] = useState(0);
  const [diagnostics, setDiagnostics] = useState<ReadDiagnosticsResponse | null>(null);
  const [error, setError] = useState<string | null>(null);

  const callRequest = async (request: Request): Promise<Response> => {
    const connection = zmkApp?.state.connection;
    if (!connection || !subsystem) {
      throw new Error("PAW3222 subsystem is not available");
    }
    const service = new ZMKCustomSubsystem(connection, subsystem.index);
    const payload = Request.encode(request).finish();
    const responsePayload = await service.callRPC(payload);
    return Response.decode(responsePayload);
  };

  const refreshInfo = async () => {
    setError(null);
    try {
      const resp = await callRequest(Request.create({ getInfo: { source: 0 } }));
      if (resp.error) {
        throw new Error(resp.error.message);
      }
      setDevices(resp.getInfo?.devices ?? []);
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    }
  };

  const refreshDiagnostics = async (deviceIndex: number) => {
    setError(null);
    try {
      const resp = await callRequest(
        Request.create({ readDiagnostics: { deviceIndex, source: 0 } })
      );
      if (resp.error) {
        throw new Error(resp.error.message);
      }
      setDiagnostics(resp.readDiagnostics ?? null);
    } catch (e) {
      setError(e instanceof Error ? e.message : "Unknown error");
    }
  };

  useEffect(() => {
    if (subsystem) {
      refreshInfo();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem]);

  return (
    <section className="card">
      <h2>Sensor Info</h2>
      {error && (
        <div className="error-message">
          <p>{error}</p>
        </div>
      )}

      {devices.length === 0 && !error && <p>No PAW3222 devices found.</p>}

      {devices.map((device) => (
        <div key={device.deviceIndex} className="sensor-entry">
          <button
            className="btn"
            onClick={() => {
              setSelectedDevice(device.deviceIndex);
              refreshDiagnostics(device.deviceIndex);
            }}
          >
            Device {device.deviceIndex}
            {device.settings_id ? ` (${device.settings_id})` : ""}
          </button>
          <p>
            ready: {device.ready ? "yes" : "no"} · product id: 0x
            {device.product_id.toString(16)} · cpi: {device.runtimeConfig?.cpi ?? "?"}
          </p>
        </div>
      ))}

      {diagnostics && (
        <div className="diagnostics">
          <h3>Diagnostics (device {selectedDevice})</h3>
          <p>
            product_id1: 0x{diagnostics.productId1.toString(16)} · product_id2: 0x
            {diagnostics.productId2.toString(16)}
          </p>
          <p>motion: 0x{diagnostics.motion.toString(16)} · cpi: {diagnostics.cpi}</p>
        </div>
      )}
    </section>
  );
}
