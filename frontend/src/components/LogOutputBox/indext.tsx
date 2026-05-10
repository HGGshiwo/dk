import React, { useState, type FC, useRef, useEffect, useMemo } from "react";
import { Radio, Card, type RadioChangeEvent } from "antd";
import "./index.css";
import { useAppStore } from "../../store/useAppStore";

export interface LogItem {
  id: string;
  type: string;
  content: string;
  time: number;
}
export interface LogOutputBoxProps {
  initialLogs?: LogItem[];
  logListHeight?: string | number;
  title?: string;
  maxLogCount?: number;
}

const formatTime = (timestamp: number) =>
  new Date(timestamp).toLocaleString("zh-CN", {
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
const scrollToTop = (container: HTMLDivElement | null) => {
  if (container) {
    container.scrollTo({ top: container.scrollHeight, behavior: "smooth" });
  }
};

// 组件核心逻辑（完全不变）
const LogOutputBox: FC<LogOutputBoxProps> = ({
  initialLogs = [],
  logListHeight = 300,
  title = "日志输出框",
}) => {
  const { stateData, config } = useAppStore();

  const [selectedType, setSelectedType] = useState<string>("all");

  const [logList, setLogList] = useState<LogItem[]>(initialLogs);
  const [isPreventAutoScroll, setIsPreventAutoScroll] =
    useState<boolean>(false);
  const logListRef = useRef<HTMLDivElement>(null);
  const logTypeConfigs = useMemo(
    () => Object.values(config?.logbox || {}),
    [config],
  );
  useEffect(() => {
    const logListData = logTypeConfigs.map((config) => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const formatData = (data: Record<string, any>) => {
        const other = Object.entries(data).reduce((prev, [k, v]) => {
          if (k == "timestamp" || k == "type" || k == config.type) return prev;
          return `${prev} [${k}] ${v}`;
        }, "");
        return `${data[config.type]} ${other}`;
      };
      return (
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        (stateData[config.type] as any[])?.map((data) => ({
          time: data["timestamp"],
          content: formatData(data),
          type: config.type,
          id: String(data["timestamp"]),
        })) || []
      );
    });

    const logListDataFlatten = logListData
      .reduce((prev, cur) => [...prev, ...cur], [])
      .sort((a, b) => a.time - b.time);

    // eslint-disable-next-line react-hooks/set-state-in-effect
    setLogList(logListDataFlatten);
  }, [stateData, logTypeConfigs]);

  const handlePreventScroll = () => setIsPreventAutoScroll(true);
  const handleAllowScroll = () => {
    setIsPreventAutoScroll(false);
    scrollToTop(logListRef.current);
  };
  const handleLongPress = (e: React.MouseEvent | React.TouchEvent) => {
    e.preventDefault();
    setIsPreventAutoScroll(true);
  };
  const handleLongPressEnd = () => setIsPreventAutoScroll(false);
  const handleTypeChange = (e: RadioChangeEvent) => {
    setSelectedType(e.target.value);
  };

  const filteredLogList = useMemo(() => {
    if (selectedType === "all") return logList;
    return logList.filter((log) => log.type === selectedType);
  }, [logList, selectedType]);

  useEffect(() => {
    if (!isPreventAutoScroll) {
      scrollToTop(logListRef.current);
    }
  }, [logList, isPreventAutoScroll]);

  const getLogConfigByType = (type: string) => {
    return logTypeConfigs?.find((c) => c.type === type) || logTypeConfigs![0];
  };

  return (
    <Card
      className="mb-2 w-full"
      title={title}
      style={{ boxShadow: "0 2px 8px rgba(0,0,0,0.08)" }}
    >
      <div className="log-type-selector">
        <span className="selector-label">日志类型：</span>
        <Radio.Group
          value={selectedType}
          onChange={handleTypeChange}
          size="large"
          optionType="button"
        >
          <Radio value="all">全部</Radio>
          {logTypeConfigs?.map((config) => (
            <Radio key={config.type} value={config.type}>
              {config.label}
            </Radio>
          ))}
        </Radio.Group>
      </div>
      <div
        ref={logListRef}
        className="log-list"
        style={{ height: logListHeight }}
        tabIndex={0}
        onFocus={handlePreventScroll}
        onBlur={handleAllowScroll}
        onMouseEnter={handlePreventScroll}
        onMouseLeave={handleAllowScroll}
        onContextMenu={handleLongPress}
        onMouseUp={handleLongPressEnd}
        onTouchEnd={handleLongPressEnd}
      >
        {filteredLogList.length === 0 ? (
          <div className="empty-log">暂无日志</div>
        ) : (
          filteredLogList.map((log, idx) => {
            const logConfig = getLogConfigByType(log.type);
            return (
              <div key={idx} className="log-item">
                <span
                  className="log-type-tag"
                  style={{
                    color: logConfig.color,
                    backgroundColor: logConfig.backgroundColor,
                    borderColor: logConfig.borderColor,
                  }}
                >
                  {logConfig.label}
                </span>
                <span className="log-time">{formatTime(log.time)}</span>
                <span className="log-content">{log.content}</span>
              </div>
            );
          })
        )}
      </div>
    </Card>
  );
};

export default LogOutputBox;
