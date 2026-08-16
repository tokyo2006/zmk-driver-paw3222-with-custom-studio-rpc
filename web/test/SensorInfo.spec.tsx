import { render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { SensorInfo, PAW3222_SUBSYSTEM_IDENTIFIER } from "../src/SensorInfo";

describe("SensorInfo Component", () => {
  describe("With Subsystem", () => {
    it("should render sensor and diagnostics cards when subsystem is found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [PAW3222_SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <SensorInfo />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: /Sensor/i })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("heading", { name: /Diagnostics/i })
      ).toBeInTheDocument();
    });
  });

  describe("Without Subsystem", () => {
    it("should show a warning when the subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <SensorInfo />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "cormoran__paw3222" not found/i)
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<SensorInfo />);

      expect(container.firstChild).toBeNull();
    });
  });
});
