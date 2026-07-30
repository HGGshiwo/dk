import { create } from "zustand";
import { message } from "antd";
import { type GlobalConfig } from "../config";
import { httpRequest, sortByOrder, wsURL } from "../utils";
import type { TableRowData } from "../components/Table";
import { type WaypointData } from "../components/WaypointEditor";

export interface LogItemData {
  timestamp: number;
  content: string;
}

type WsStatus = "connecting" | "open" | "closed" | "error";

interface AppState {
  stateData: Record<
    string,
    LogItemData[] | string | TableRowData | number | WaypointData[]
  >;
  wsStatus: WsStatus;
  modalVisible: boolean;
  modalKey: string;
  modalFormData: Record<string, unknown>;
  config: GlobalConfig | null;
  isLoading: boolean;
  lastMessageTime: number;

  setModalVisible: (visible: boolean, key?: string) => void;
  updateModalFormData: (data: Record<string, unknown> | object) => void;
  resetModalFormData: () => void;
  initApp: () => void;
  logboxVisible: boolean;
  setLogboxVisible: (visible: boolean) => void;
}

let wsInstance: WebSocket | null = null;
let reconnectTimer: number | null = null;
let isManualClose = false;
const wsListeners: ((data: any) => void)[] = [];
let fglogBuffer: any[] = [];

export const getFglogBuffer = () => {
  return fglogBuffer;
};

export const fglogHandlerRef: { current: ((data: any) => void) | null } = { current: null };

// 2. 导出设置和移除的方法
export const setFglogHandler = (listener: (data: any) => void) => {
  fglogHandlerRef.current = listener;
  console.log("set fglogger");
};

export const removeFglogHandler = () => {
  fglogHandlerRef.current = null;
  console.log("remove fglogger");
};


export const addWsListener = (listener: (data: any) => void) => {
  wsListeners.push(listener);
};

export const removeWsListener = (listener: (data: any) => void) => {
  const index = wsListeners.indexOf(listener);
  if (index !== -1) {
    wsListeners.splice(index, 1);
  }
};

export const clearLocalStorageHistory = () => {
  fglogBuffer = [];
  try {
    const countStr = localStorage.getItem("fglog_chunk_count");
    if (countStr) {
      const count = parseInt(countStr, 10);
      for (let i = 0; i < count; i++) {
        localStorage.removeItem(`fglog_chunk_${i}`);
      }
    }
    localStorage.setItem("fglog_chunk_count", "0");
  } catch (e) {
    console.error("Failed to clear localStorage history:", e);
  }
};

const handleFglog = (data: any) => {
  if (data.type != "fglog") return false;
  const fglog = data.fglog;
  if (!fglog) return false;

  const { time, topic, value } = fglog;
  if (time !== undefined && topic !== undefined && value !== undefined) {
    const dataPoint: any = { time };
    if (Array.isArray(value)) {
      if (value.length >= 3) {
        dataPoint[topic + ".x"] = value[0];
        dataPoint[topic + ".y"] = value[1];
        dataPoint[topic + ".z"] = value[2];
      } else if (value.length === 2) {
        dataPoint[topic + ".x"] = value[0];
        dataPoint[topic + ".y"] = value[1];
      }
    } else {
      if (fglog.type === "log") {
        dataPoint.log = value;
      } else {
        dataPoint[topic] = value;
      }
    }

    fglogBuffer.push(dataPoint);

    if (fglogBuffer.length >= 2000) {
      const chunk = fglogBuffer.slice(0, 1000);
      fglogBuffer = fglogBuffer.slice(1000);
      try {
        const countStr = localStorage.getItem("fglog_chunk_count") || "0";
        const count = parseInt(countStr, 10);
        localStorage.setItem(`fglog_chunk_${count}`, JSON.stringify(chunk));
        localStorage.setItem("fglog_chunk_count", (count + 1).toString());
      } catch (e) {
        console.error("Failed to write chunk to localStorage:", e);
      }
    }
  }
  fglogHandlerRef.current?.(fglog);
  return true;
}

const initWebSocket = () => {
  if (wsInstance) {
    wsInstance.close();
    wsInstance = null;
  }

  isManualClose = false;
  useAppStore.setState({ wsStatus: "connecting" });

  try {
    const ws = new WebSocket(wsURL);

    ws.onopen = () => {
      console.log("WebSocket连接成功");
      useAppStore.setState({ wsStatus: "open" });
      // fglog_enable will be explicitly triggered by the user via the visualizer UI
      if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
      }
    };

    ws.onclose = (event) => {
      console.log("WebSocket连接关闭", event);
      useAppStore.setState({ wsStatus: "closed" });
      if (!isManualClose) {
        scheduleReconnect();
      }
    };

    ws.onerror = (error) => {
      console.error("WebSocket连接错误", error);
      useAppStore.setState({ wsStatus: "error" });
      if (!isManualClose) {
        scheduleReconnect();
      }
    };

    ws.onmessage = (event) => {
      useAppStore.setState({ lastMessageTime: Date.now() });
      try {
        const data = JSON.parse(event.data);
        if (handleFglog(data)) {
          return;
        }

        wsListeners.forEach((listener) => {
          try {
            listener(data);
          } catch (e) {
            console.error("WS listener error:", e);
          }
        });

        if (!data.type) return;

        const dataType = data.type;
        delete data.type;

        const currentState = useAppStore.getState().stateData;

        if (dataType === "state") {
          useAppStore.setState({ stateData: { ...currentState, ...data } });
        } else {
          if (dataType === "error") {
            message.error(data?.error);
          }
          const currentList = (currentState[dataType] || []) as LogItemData[];
          useAppStore.setState({
            stateData: { ...currentState, [dataType]: [...currentList, data] },
          });
        }
        if (data.msg_id != undefined) {
          ws.send(JSON.stringify({ msg_id: data.msg_id }))
        }
      } catch (error) {
        console.error("WS消息解析失败：", error);
      }
    };

    wsInstance = ws;
  } catch (error) {
    console.error("创建WebSocket实例失败", error);
    useAppStore.setState({ wsStatus: "error" });
    scheduleReconnect();
  }
};

const scheduleReconnect = () => {
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
  }
  reconnectTimer = setTimeout(() => {
    console.log("尝试重新连接WebSocket...");
    initWebSocket();
  }, 3000) as unknown as number;
};

export const useAppStore = create<AppState>((set, get) => ({
  stateData: {},
  wsStatus: "connecting",
  modalVisible: false,
  modalKey: "",
  modalFormData: {},
  config: null,
  isLoading: true,
  logboxVisible: false,
  lastMessageTime: 0,

  setLogboxVisible: (visible) => set({ logboxVisible: visible }),

  setModalVisible: (visible, key) => {
    set({ modalVisible: visible });
    if (key) set({ modalKey: key });
    if (!visible) get().resetModalFormData();
  },

  updateModalFormData: (data) => {
    set((state) => ({ modalFormData: { ...state.modalFormData, ...data } }));
  },

  resetModalFormData: () => {
    set({ modalFormData: {}, modalKey: "" });
  },

  initApp: async () => {
    try {
      console.log("init");
      const res = await httpRequest("GET", "/page_config");
      const config = (res || null) as GlobalConfig;
      set({ config, isLoading: false });

      if (config?.state) {
        const stateEntries = sortByOrder(config.state);
        const initStateData = stateEntries.reduce(
          (prev, item) => {
            const key = item.key;
            prev[key] = item.default;
            return prev;
          },
          {} as Record<string, string>,
        );

        // 初始化 state_bool 的默认值
        if (config?.state_bool) {
          const stateBoolEntries = sortByOrder(config.state_bool);
          stateBoolEntries.forEach((item) => {
            initStateData[item.key] = item.default;
          });
        }

        // 初始化 state_value 的默认值
        if (config?.state_value) {
          const stateValueEntries = sortByOrder(config.state_value);
          stateValueEntries.forEach((item) => {
            initStateData[item.key] = item.default;
          });
        }

        set({ stateData: initStateData });
      }

      initWebSocket();
    } catch (error) {
      console.error("加载页面配置失败", error);
      set({ isLoading: false });
    }
  },
}));

export const cleanupWebSocket = () => {
  isManualClose = true;
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  if (wsInstance) {
    wsInstance.close();
    wsInstance = null;
  }
};

export const sendWebSocketMessage = (message: any) => {
  if (wsInstance && wsInstance.readyState === WebSocket.OPEN) {
    try {
      wsInstance.send(typeof message === "string" ? message : JSON.stringify(message));
      return true;
    } catch (err) {
      console.error("发送WS消息失败", err);
    }
  }
  return false;
};
