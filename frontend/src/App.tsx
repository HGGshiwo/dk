import { useEffect, useState } from "react";
import { WSStatus } from "./components/WSStatus";
import { StateDisplay } from "./components/StateDisplay";
import { ButtonGroup } from "./components/ButtonGroup";
import { useAppStore, cleanupWebSocket } from "./store/useAppStore";
import { VirtualJoystickGroup } from "./components/VirtualJoystick";
import LogOutputBox from "./components/LogOutputBox/indext";
import { ConfigProvider, Spin, Modal, type ThemeConfig } from "antd";
import WaypointEditor from "./components/WaypointEditor";

function App() {
  const [isWaypointEditing, setIsWaypointEditing] = useState(false);
  
  const theme: ThemeConfig = {
    components: {
      Card: { headerFontSize: "18px" },
    },
  };

  const initApp = useAppStore((state) => state.initApp);
  const wsStatus = useAppStore((state) => state.wsStatus);
  const logboxVisible = useAppStore((state) => state.logboxVisible);
  const setLogboxVisible = useAppStore((state) => state.setLogboxVisible);
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
      style={{ width: '100%', height: '100%' }}
    >
      <ConfigProvider theme={theme}>
        <div style={{ width: '100vw', height: '100vh', position: 'relative', overflow: 'hidden' }}>
          {/* Canvas全屏背景 */}
          <div style={{ position: 'absolute', top: 0, left: 0, right: 0, bottom: 0, zIndex: 0 }}>
            <WaypointEditor onEditModeChange={setIsWaypointEditing} />
          </div>
          
          {/* 顶部状态显示 - 绝对定位 */}
          <div style={{ position: 'absolute', top: 0, left: 0, right: 0, zIndex: 10, pointerEvents: 'none' }}>
            <WSStatus />
            <StateDisplay />
          </div>
          
          {/* 底部按钮卡片 - 绝对定位，编辑模式时隐藏 */}
          {!isWaypointEditing && (
            <div style={{ position: 'absolute', bottom: 0, left: 0, right: 0, zIndex: 20, padding: '16px 16px 32px 16px' }}>
              <ButtonGroup />
            </div>
          )}
          
          {/* 隐藏的组件 - 如果需要可以调整 */}
          <div className="hidden">
            <VirtualJoystickGroup />
          </div>
          
          <Modal
            open={logboxVisible}
            onCancel={() => setLogboxVisible(false)}
            footer={null}
            width={800}
            destroyOnClose={false}
          >
            <LogOutputBox title={null} />
          </Modal>
        </div>
      </ConfigProvider>
    </Spin>
  );
}

export default App;
