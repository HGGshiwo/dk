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
