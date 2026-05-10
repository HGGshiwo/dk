import { useEffect } from "react";
import { WSStatus } from "./components/WSStatus";
import { StateDisplay } from "./components/StateDisplay";
import { ButtonGroup } from "./components/ButtonGroup";
import { useAppStore, cleanupWebSocket } from "./store/useAppStore";
import { VirtualJoystickGroup } from "./components/VirtualJoystick";
import LogOutputBox from "./components/LogOutputBox/indext";
import { ConfigProvider, Spin, type ThemeConfig } from "antd";
import WaypointEditor from "./components/WaypointEditor";

function App() {
  const theme: ThemeConfig = {
    components: {
      Card: { headerFontSize: "18px" },
    },
  };

  const initApp = useAppStore((state) => state.initApp);
  const wsStatus = useAppStore((state) => state.wsStatus);
  useEffect(() => {
    initApp();
    return () => {
      cleanupWebSocket();
    };
  }, [initApp]);

  return (
    <Spin
      spinning={wsStatus !== "open"}
      delay={500}
      size="large"
      tip="尝试websocket连接中..."
    >
      <ConfigProvider theme={theme}>
        <div className="w-full h-full flex flex-col gap-2">
          <WSStatus />
          <ButtonGroup />
          <StateDisplay />
          <VirtualJoystickGroup />
          <LogOutputBox /> 
          <WaypointEditor />
        </div>
      </ConfigProvider>
    </Spin>
  );
}

export default App;
