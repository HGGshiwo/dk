import React, { useState, useEffect } from "react";
import { Modal, Button, Radio, Upload } from "antd";
import { CloseOutlined, SyncOutlined, DeleteOutlined, UploadOutlined } from "@ant-design/icons";
import { useAppStore, sendWebSocketMessage } from "../../store/useAppStore";
import { useVisualizerData, type DataPoint, type SourceConfig } from "./useVisualizerData";
import { SourceControlTable } from "./SourceControlTable";
import { CanvasRenderer } from "./CanvasRenderer";

interface FoxgloveVisualizerProps {
  visible: boolean;
  onClose: () => void;
}

export const FoxgloveVisualizer: React.FC<FoxgloveVisualizerProps> = ({ visible, onClose }) => {
  const wsStatus = useAppStore((state) => state.wsStatus);
  const [isLocalMode, setIsLocalMode] = useState<boolean>(wsStatus !== "open");
  const [isPlaying, setIsPlaying] = useState<boolean>(true);
  const [timeWindow, setTimeWindow] = useState<number>(30);
  const [dragOffset, setDragOffset] = useState<number>(0);
  const [zoomFactors, setZoomFactors] = useState<Record<string, number>>({});

  const {
    sources,
    setSources,
    sourcesRef,
    mergedDataRef,
    lastTimeRef,
    clearData,
  } = useVisualizerData(visible, isLocalMode, isPlaying);

  // 网络状态自动切换
  useEffect(() => {
    if (wsStatus === "open") {
      setIsLocalMode(false);
      setIsPlaying(true);
    } else {
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

  // 本地文件读取解析
  const handleLocalFile = (file: File) => {
    const reader = new FileReader();
    reader.onload = (e) => {
      const text = e.target?.result as string;
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
            const { time, topic, value } = entry;
            maxTime = Math.max(maxTime, time);

            if (!newSourcesMap.has(topic)) {
              const color = colorList[newSourcesMap.size % colorList.length];
              const type = typeof value === "number" ? "line" : topic.includes("log") ? "log" : "state";
              newSourcesMap.set(topic, { id: topic, name: topic.split("/").pop() || topic, topic, type, enabled: true, autoY: true, locked: false, color });
            }

            loadedPoints.push({ time, [topic]: value });
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
    reader.readAsText(file);
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
            <div className="flex items-center gap-2">
              <Radio.Group
                value={isLocalMode}
                onChange={(e) => {
                  const val = e.target.value;
                  setIsLocalMode(val);
                  if (!val) {
                    clearData();
                    sendWebSocketMessage({ fglog_enable: true });
                    setIsPlaying(true);
                  } else {
                    setIsPlaying(false);
                  }
                }}
                size="small"
                buttonStyle="solid"
              >
                <Radio.Button value={false} disabled={wsStatus !== "open"}>实时WebSocket数据</Radio.Button>
                <Radio.Button value={true}>本地日志文件</Radio.Button>
              </Radio.Group>

              {isLocalMode && (
                <Upload
                  beforeUpload={(file) => {
                    handleLocalFile(file);
                    return false;
                  }}
                  showUploadList={false}
                  accept=".mcap,.json,.log,.txt"
                >
                  <Button icon={<UploadOutlined />} size="small" type="dashed">
                    选择日志文件
                  </Button>
                </Upload>
              )}
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