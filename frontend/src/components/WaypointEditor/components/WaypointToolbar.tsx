import React from "react";
import { Button } from "antd";
import {
  AimOutlined,
  EditOutlined,
  CloseOutlined,
  ControlOutlined,
} from "@ant-design/icons";

interface WaypointToolbarProps {
  showWaypointPanel: boolean;
  setShowWaypointPanel: (show: boolean) => void;
  showJoystick: boolean;
  setShowJoystick: (show: boolean) => void;
  onReset: () => void;
}

export const WaypointToolbar: React.FC<WaypointToolbarProps> = ({
  showWaypointPanel,
  setShowWaypointPanel,
  showJoystick,
  setShowJoystick,
  onReset,
}) => {
  return (
    <div
      style={{
        position: "absolute",
        top: "50%",
        right: "16px",
        transform: "translateY(-50%)",
        zIndex: 5,
        display: "flex",
        flexDirection: "column",
        gap: "16px",
      }}
    >
      <div
        style={{
          display: "flex",
          flexDirection: "column",
          alignItems: "center",
          gap: "4px",
        }}
      >
        <Button
          shape="circle"
          icon={<AimOutlined />}
          onClick={onReset}
          style={{ boxShadow: "0 4px 12px rgba(0,0,0,0.15)" }}
          title="将十字中心移到飞机当前位置"
        />
        <span style={{ fontSize: "12px", color: "#666", fontWeight: 500 }}>
          重置
        </span>
      </div>

      <div
        style={{
          display: "flex",
          flexDirection: "column",
          alignItems: "center",
          gap: "4px",
        }}
      >
        <Button
          type={showWaypointPanel ? "default" : "primary"}
          shape="circle"
          icon={showWaypointPanel ? <CloseOutlined /> : <EditOutlined />}
          onClick={() => setShowWaypointPanel(!showWaypointPanel)}
          style={{ boxShadow: "0 4px 12px rgba(0,0,0,0.15)" }}
        />
        <span style={{ fontSize: "12px", color: "#666", fontWeight: 500 }}>
          {showWaypointPanel ? "关闭" : "航点"}
        </span>
      </div>

      <div
        style={{
          display: "flex",
          flexDirection: "column",
          alignItems: "center",
          gap: "4px",
        }}
      >
        <Button
          type={showJoystick ? "default" : "primary"}
          shape="circle"
          icon={showJoystick ? <CloseOutlined /> : <ControlOutlined />}
          onClick={() => setShowJoystick(!showJoystick)}
          style={{ boxShadow: "0 4px 12px rgba(0,0,0,0.15)" }}
        />
        <span style={{ fontSize: "12px", color: "#666", fontWeight: 500 }}>
          {showJoystick ? "关闭" : "摇杆"}
        </span>
      </div>
    </div>
  );
};
