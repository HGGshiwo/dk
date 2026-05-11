import type { ButtonItemConfig } from "../components/ButtonGroup";
import type { StateItemConfig } from "../components/StateDisplay";
import type { TableGroupConfig } from "../components/TableGroup";

// 确认按钮配置接口
export interface ConfirmItemConfig {
  key: string;
  target: string; // 关联的click按钮key
  method: "GET" | "POST" | "PUT" | "DELETE";
  url: string;
}

export interface JoystickItemConfig {
  key: string;
  method: "GET" | "POST" | "PUT" | "DELETE";
  url: string;
}

export interface WaypointVisConfig {
  waypointApi: {
    method: "GET" | "POST" | "PUT" | "DELETE";
    url: string;
  };
  followApi: {
    method: "GET" | "POST" | "PUT" | "DELETE";
    url: string;
    interval?: number;
  };
}

// 全局配置接口
export interface GlobalConfig {
  state: StateItemConfig[]; // 状态展示配置
  button: ButtonItemConfig[]; // 控制按钮配置
  joystick: JoystickItemConfig[]; //虚拟摇杆操作配置
  logbox: LogTypeConfig[]; // 日志框的配置
  table: TableGroupConfig[]; // 表格的配置
  waypointVis?: WaypointVisConfig;
  key: string;
}

// 核心类型定义（完全不变）
export interface LogTypeConfig {
  type: string;
  label: string;
  color: string;
  borderColor: string;
  backgroundColor: string;
}
