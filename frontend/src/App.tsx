import { useEffect, useState } from "react";
import { WSStatus } from "./components/WSStatus";
import { StateDisplay } from "./components/StateDisplay";
import { ButtonGroup } from "./components/ButtonGroup";
import { useAppStore, cleanupWebSocket } from "./store/useAppStore";
import { VirtualJoystickGroup } from "./components/VirtualJoystick";
import LogOutputBox from "./components/LogOutputBox/indext";
import { ConfigProvider, Spin, Modal, Button, Tooltip, type ThemeConfig } from "antd";
import { AreaChartOutlined } from "@ant-design/icons";
import WaypointEditor from "./components/WaypointEditor";
import { FoxgloveVisualizer } from "./components/FoxgloveVisualizer";

function App() {
  const [isWaypointEditing, setIsWaypointEditing] = useState(false);
  const [isVisualizerOpen, setIsVisualizerOpen] = useState(false);
  
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
    <ConfigProvider theme={theme}>
      <div style={{ width: '100vw', height: '100vh', position: 'relative', overflow: 'hidden' }}>
        
        {/* WebSocket Connection Overlay */}
        {wsStatus !== "open" && (
          <div style={{
            position: 'absolute',
            top: 0,
            left: 0,
            right: 0,
            bottom: 0,
            backgroundColor: 'rgba(15, 23, 42, 0.85)', // slate-900 with opacity
            backdropFilter: 'blur(8px)',
            zIndex: 100, // Above normal UI elements, below standard Modal zIndex (1000)
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            justifyContent: 'center',
            color: '#fff',
            fontFamily: 'system-ui, sans-serif'
          }}>
            <Spin size="large" />
            <h3 style={{ marginTop: '24px', marginBottom: '8px', fontSize: '18px', fontWeight: 600 }}>
              {wsStatus === "connecting" ? "正在尝试连接 WebSocket..." : "WebSocket 连接断开，尝试重连中..."}
            </h3>
            <p style={{ color: '#94a3b8', fontSize: '14px', marginBottom: '24px' }}>
              请检查网络或确认后台服务已启动。
            </p>
            <Button 
              type="primary" 
              size="large" 
              onClick={() => setIsVisualizerOpen(true)}
              style={{
                backgroundColor: '#4f46e5',
                borderColor: '#4338ca',
                boxShadow: '0 4px 12px rgba(79, 70, 229, 0.4)'
              }}
            >
              打开日志查看面板
            </Button>
          </div>
        )}

        {/* Canvas全屏背景 */}
        <div style={{ position: 'absolute', top: 0, left: 0, right: 0, bottom: 0, zIndex: 0 }}>
          <WaypointEditor onEditModeChange={setIsWaypointEditing} />
        </div>
        
        {/* 顶部状态显示 - 绝对定位 */}
        <div style={{ position: 'absolute', top: 0, left: 0, right: 0, zIndex: 10, pointerEvents: 'none' }}>
          <WSStatus />
          <StateDisplay />
        </div>
        
        {/* 左侧可视化触发按钮 */}
        {!isWaypointEditing && (
          <div style={{ position: 'absolute', left: '16px', top: '50%', transform: 'translateY(-50%)', zIndex: 30 }}>
            <Tooltip title="可视化诊断 (Foxglove 模式)" placement="right">
              <Button 
                type="primary" 
                shape="circle" 
                icon={<AreaChartOutlined style={{ fontSize: '20px' }} />} 
                size="large"
                onClick={() => setIsVisualizerOpen(true)}
                style={{ 
                  width: '48px', 
                  height: '48px', 
                  boxShadow: '0 4px 12px rgba(0,0,0,0.35)',
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  backgroundColor: '#4f46e5', // indigo-600
                  borderColor: '#4338ca' // indigo-700
                }}
              />
            </Tooltip>
          </div>
        )}
        
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

        {/* Foxglove Visualizer Modal */}
        <FoxgloveVisualizer 
          visible={isVisualizerOpen}
          onClose={() => setIsVisualizerOpen(false)}
        />
      </div>
    </ConfigProvider>
  );
}

export default App;
