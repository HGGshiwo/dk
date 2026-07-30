import React, { useState, useEffect } from "react";
import { Modal, Button, Upload, Dropdown } from "antd";
import { CloseOutlined, SyncOutlined, DeleteOutlined, UploadOutlined, PlayCircleOutlined, StopOutlined, FolderOpenOutlined } from "@ant-design/icons";
import { useAppStore, sendWebSocketMessage } from "../../store/useAppStore";
import { useVisualizerData, type DataPoint, type SourceConfig } from "./useVisualizerData";
import { SourceControlTable } from "./SourceControlTable";
import { CanvasRenderer } from "./CanvasRenderer";
import { httpRequest } from "../../utils";

interface FoxgloveVisualizerProps {
  visible: boolean;
  onClose: () => void;
}

export const FoxgloveVisualizer: React.FC<FoxgloveVisualizerProps> = ({ visible, onClose }) => {
  const wsStatus = useAppStore((state) => state.wsStatus);
  const [isLocalMode, setIsLocalMode] = useState<boolean>(true); // start in local/file mode by default
  const [isPlaying, setIsPlaying] = useState<boolean>(false);
  const [timeWindow, setTimeWindow] = useState<number>(30);
  const [dragOffset, setDragOffset] = useState<number>(0);
  const [zoomFactors, setZoomFactors] = useState<Record<string, number>>({});

  // 3 buttons state
  const [isRealtimeActive, setIsRealtimeActive] = useState<boolean>(false);
  const [serverFiles, setServerFiles] = useState<string[]>([]);
  const [loadingServerFiles, setLoadingServerFiles] = useState<boolean>(false);
  const [selectedServerFile, setSelectedServerFile] = useState<string | undefined>(undefined);

  const {
    sources,
    setSources,
    sourcesRef,
    mergedDataRef,
    lastTimeRef,
    clearData,
  } = useVisualizerData(visible, isLocalMode, isPlaying);

  // 组件销毁或关闭时，发送 fglog_enable: false 关闭实时记录
  useEffect(() => {
    return () => {
      sendWebSocketMessage({ fglog_enable: false });
    };
  }, []);

  // 网络状态自动切换 (当断开时关闭实时)
  useEffect(() => {
    if (wsStatus !== "open") {
      setIsRealtimeActive(false);
      setIsLocalMode(true);
      setIsPlaying(false);
    }
  }, [wsStatus]);

  useEffect(() => {
    if (isLocalMode) setIsPlaying(false);
    else setIsPlaying(dragOffset === 0);
  }, [isLocalMode, dragOffset]);

  // 操作控制 Handler
  const handleToggleEnabled = (id: string, checked: boolean) => {
    setSources((prev) => {
      const next = prev.map((s) => (s.id === id ? { ...s, enabled: checked } : s));
      sourcesRef.current = next;
      return next;
    });
  };

  const handleToggleLock = (id: string, locked: boolean) => {
    setSources((prev) => {
      const next = prev.map((s) => (s.id === id ? { ...s, locked } : s));
      sourcesRef.current = next;
      return next;
    });
  };

  const handleResetAutoY = (id: string) => {
    setSources((prev) => {
      const next = prev.map((s) => (s.id === id ? { ...s, autoY: true } : s));
      sourcesRef.current = next;
      return next;
    });
    setZoomFactors((prev) => ({ ...prev, [id]: 1.0 }));
  };

  // 解析并加载 JSONL 日志文本内容 (根据 json fglog -> type 获取 state, log, value，不要再自动推理)
  const loadJsonlText = (text: string) => {
    if (!text) return;
    const lines = text.split("\n");
    const loadedPoints: DataPoint[] = [];
    const newSourcesMap = new Map<string, SourceConfig>();
    sources.forEach((s) => newSourcesMap.set(s.topic, s));

    let maxTime = 0;
    const colorList = ["#2563eb", "#dc2626", "#16a34a", "#eab308", "#8b5cf6", "#d946ef", "#06b6d4"];

    lines.forEach((line) => {
      if (!line.trim()) return;
      try {
        const entry = JSON.parse(line);
        if (entry.time !== undefined && entry.topic !== undefined && entry.value !== undefined) {
          const time = entry.time / 1000.0;
          const { topic, value } = entry;
          maxTime = Math.max(maxTime, time);

          const isVector = Array.isArray(value);
          if (isVector) {
            if (value.length >= 3) {
              const keys = [topic + ".x", topic + ".y", topic + ".z"];
              keys.forEach((k, idx) => {
                if (!newSourcesMap.has(k)) {
                  const color = colorList[(newSourcesMap.size + idx) % colorList.length];
                  newSourcesMap.set(k, { id: k, name: k.split("/").pop() || k, topic: k, type: "line", enabled: true, autoY: true, locked: false, color });
                }
              });
            } else if (value.length === 2) {
              const keys = [topic + ".x", topic + ".y"];
              keys.forEach((k, idx) => {
                if (!newSourcesMap.has(k)) {
                  const color = colorList[(newSourcesMap.size + idx) % colorList.length];
                  newSourcesMap.set(k, { id: k, name: k.split("/").pop() || k, topic: k, type: "line", enabled: true, autoY: true, locked: false, color });
                }
              });
            }
          } else {
            if (!newSourcesMap.has(topic)) {
              const color = colorList[newSourcesMap.size % colorList.length];
              const type = entry.type === "value" ? "line" : (entry.type === "log" ? "log" : "state");
              newSourcesMap.set(topic, { id: topic, name: topic.split("/").pop() || topic, topic, type, enabled: true, autoY: true, locked: false, color });
            }
          }

          const dataPoint: DataPoint = { time };
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
            if (entry.type === "log") {
              dataPoint.log = value;
            } else {
              dataPoint[topic] = value;
            }
          }
          loadedPoints.push(dataPoint);
        }
      } catch { }
    });

    const nextSources = Array.from(newSourcesMap.values());
    setSources(nextSources);
    sourcesRef.current = nextSources;

    loadedPoints.sort((a, b) => a.time - b.time);
    mergedDataRef.current = loadedPoints;
    lastTimeRef.current = maxTime;
    setDragOffset(0);
  };

  // 本地文件读取解析
  const handleLocalFile = (file: File) => {
    const reader = new FileReader();
    reader.onload = (e) => {
      const text = e.target?.result as string;
      loadJsonlText(text);
    };
    reader.readAsText(file);
  };

  // 1. 实时按钮逻辑 (实时 / 关闭)
  const handleToggleRealtime = () => {
    if (isRealtimeActive) {
      sendWebSocketMessage({ fglog_enable: false });
      setIsRealtimeActive(false);
      setIsPlaying(false);
    } else {
      clearData();
      sendWebSocketMessage({ fglog_enable: true });
      setIsRealtimeActive(true);
      setIsLocalMode(false);
      setIsPlaying(true);
      setSelectedServerFile(undefined); // 清除已选择的服务器文件
    }
  };

  // 2. 浏览服务器文件逻辑
  const fetchServerFiles = async () => {
    setLoadingServerFiles(true);
    try {
      const res = await httpRequest<string[]>("GET", "/fglog");
      if (Array.isArray(res)) {
        const sorted = [...res].sort((a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' }));
        setServerFiles(sorted);
        if (sorted.length > 0 && !selectedServerFile) {
          handleSelectServerFile(sorted[sorted.length - 1]);
        }
      }
    } catch (err) {
      console.error("获取服务器 fglog 列表失败", err);
    } finally {
      setLoadingServerFiles(false);
    }
  };

  const handleSelectServerFile = async (filename: string) => {
    if (isRealtimeActive) {
      sendWebSocketMessage({ fglog_enable: false });
      setIsRealtimeActive(false);
    }
    setIsLocalMode(true);
    setIsPlaying(false);
    setSelectedServerFile(filename);

    try {
      const res = await httpRequest<string | any>("GET", "/fglog", { file: filename });
      let content = "";
      if (typeof res === "string") {
        content = res;
      } else if (res && typeof res === "object") {
        content = res.content || JSON.stringify(res);
      }
      loadJsonlText(content);
    } catch (err) {
      console.error(`读取服务器 fglog 文件 ${filename} 失败`, err);
    }
  };

  return (
    <Modal
      open={visible}
      onCancel={onClose}
      footer={null}
      width="100vw"
      destroyOnClose
      styles={{
        container: { padding: 0, borderRadius: 0, height: "100vh", width: "100vw", display: "flex", flexDirection: "column" },
        body: { padding: 0, flex: 1, height: "100%", display: "flex", flexDirection: "column", overflow: "hidden" },
      }}
      style={{ top: 0, margin: 0, padding: 0, maxWidth: "100vw", height: "100vh" }}
      closeIcon={<CloseOutlined style={{ color: "#475569", fontSize: "16px" }} />}
    >
      <div className="flex flex-col md:flex-row h-full overflow-hidden text-sm">
        {/* 左侧数据源面板 */}
        <SourceControlTable
          sources={sources}
          onToggleEnabled={handleToggleEnabled}
          onToggleLock={handleToggleLock}
          onResetAutoY={handleResetAutoY}
        />

        {/* 右侧绘图及控制工具栏 */}
        <div className="flex-1 flex flex-col h-[60vh] md:h-full overflow-hidden p-3 bg-white">
          <CanvasRenderer
            visible={visible}
            sources={sources}
            sourcesRef={sourcesRef}
            mergedDataRef={mergedDataRef}
            lastTimeRef={lastTimeRef}
            dragOffset={dragOffset}
            setDragOffset={setDragOffset}
            timeWindow={timeWindow}
            setTimeWindow={setTimeWindow}
            setIsPlaying={setIsPlaying}
            zoomFactors={zoomFactors}
            setZoomFactors={setZoomFactors}
            setSources={setSources}
          />

          {/* 底部工具条 */}
          <div className="flex flex-wrap justify-between items-center gap-3 bg-slate-50 border border-slate-200 p-2.5 rounded mt-2.5 text-xs">
            <div className="flex flex-wrap items-center gap-2">
              {/* 第一个按钮: 实时 */}
              {isRealtimeActive ? (
                <Button 
                  size="small" 
                  type="primary" 
                  danger 
                  icon={<StopOutlined />} 
                  onClick={handleToggleRealtime}
                >
                  关闭
                </Button>
              ) : (
                <Button 
                  size="small" 
                  type="primary" 
                  icon={<PlayCircleOutlined />} 
                  onClick={handleToggleRealtime} 
                  disabled={wsStatus !== "open"}
                >
                  实时
                </Button>
              )}

              {/* 第二个按钮: 浏览服务器文件 */}
              <Dropdown
                menu={{
                  items: serverFiles.map((file) => ({
                    key: file,
                    label: file,
                  })),
                  onClick: ({ key }) => handleSelectServerFile(key),
                }}
                trigger={["click"]}
                placement="top"
              >
                <Button 
                  size="small" 
                  onClick={fetchServerFiles} 
                  loading={loadingServerFiles} 
                  icon={<FolderOpenOutlined />}
                >
                  {selectedServerFile || "远程文件"}
                </Button>
              </Dropdown>

              {/* 第三个按钮: 浏览本地文件 */}
              <Upload
                beforeUpload={(file) => {
                  handleLocalFile(file);
                  return false;
                }}
                showUploadList={false}
              >
                <Button icon={<UploadOutlined />} size="small" type="dashed">
                  本地文件
                </Button>
              </Upload>
            </div>

            {dragOffset > 0 ? (
              <Button
                size="small"
                icon={<SyncOutlined className="animate-spin text-xs" />}
                onClick={() => {
                  setDragOffset(0);
                  setIsPlaying(true);
                }}
                className="h-6 text-xs px-2.5 bg-emerald-50 text-emerald-600 border-emerald-200"
              >
                回到实时
              </Button>
            ) : (
              <span className="text-slate-400 font-sans pl-1 hidden sm:inline">滑动绘图区或滚动数轴查看波形</span>
            )}

            <Button size="small" danger icon={<DeleteOutlined />} onClick={clearData} className="h-6 text-xs px-2.5">
              清空
            </Button>
          </div>
        </div>
      </div>
    </Modal>
  );
};