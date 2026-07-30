import React, { useEffect, useState, useRef, useMemo } from "react";
import { Modal, Select, Button, Input, Space, Spin } from "antd";
import { ReloadOutlined, FileTextOutlined, ArrowDownOutlined, ArrowUpOutlined } from "@ant-design/icons";
import { httpRequest } from "../../utils";

interface SpdLogViewerProps {
  visible: boolean;
  onClose: () => void;
}

interface ParsedLogLine {
  original: string;
  time?: string;
  level?: string;
  thread?: string;
  message?: string;
}

export const SpdLogViewer: React.FC<SpdLogViewerProps> = ({ visible, onClose }) => {
  const [fileList, setFileList] = useState<string[]>([]);
  const [selectedFile, setSelectedFile] = useState<string | undefined>(undefined);
  const [logLines, setLogLines] = useState<ParsedLogLine[]>([]);
  const [loading, setLoading] = useState<boolean>(false);
  const [loadingList, setLoadingList] = useState<boolean>(false);
  const [searchQuery, setSearchQuery] = useState<string>("");

  const consoleContainerRef = useRef<HTMLDivElement>(null);

  // Fetch list of files
  const fetchFileList = async () => {
    setLoadingList(true);
    try {
      const res = await httpRequest<string[]>("GET", "/spdlog");
      if (Array.isArray(res)) {
        const sorted = [...res].sort((a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' }));
        setFileList(sorted);
        if (sorted.length > 0 && !selectedFile) {
          setSelectedFile(sorted[sorted.length - 1]);
        }
      }
    } catch (err) {
      console.error("获取 spdlog 列表失败", err);
    } finally {
      setLoadingList(false);
    }
  };

  // Fetch content of selected file
  const fetchFileContent = async (filename: string) => {
    setLoading(true);
    try {
      const res = await httpRequest<string | any>("GET", "/spdlog", { file: filename });
      
      // Parse file content
      let content = "";
      if (typeof res === "string") {
        content = res;
      } else if (res && typeof res === "object") {
        content = res.content || JSON.stringify(res);
      }
      
      const lines = content.split("\n");
      const parsed: ParsedLogLine[] = lines.map(line => {
        if (!line.trim()) return { original: line };
        // Pattern: [%Y-%m-%d %H:%M:%S.%e] [%l] [thread %t] %v
        // Example: [2026-07-30 11:20:35.123] [info] [thread 123] message
        const match = line.match(/^\[([^\]]+)\]\s+\[([^\]]+)\]\s+\[thread\s+([^\]]+)\]\s+(.*)$/);
        if (match) {
          return {
            original: line,
            time: match[1],
            level: match[2].toLowerCase(),
            thread: match[3],
            message: match[4]
          };
        }
        return { original: line };
      });
      
      setLogLines(parsed);
    } catch (err) {
      console.error(`读取 spdlog 文件 ${filename} 失败`, err);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    if (visible) {
      fetchFileList();
    }
  }, [visible]);

  useEffect(() => {
    if (selectedFile) {
      fetchFileContent(selectedFile);
    } else {
      setLogLines([]);
    }
  }, [selectedFile]);



  const handleScrollToTop = () => {
    if (consoleContainerRef.current) {
      consoleContainerRef.current.scrollTo({ top: 0, behavior: "smooth" });
    }
  };

  const handleScrollToBottom = () => {
    if (consoleContainerRef.current) {
      consoleContainerRef.current.scrollTo({
        top: consoleContainerRef.current.scrollHeight,
        behavior: "smooth"
      });
    }
  };

  const filteredLines = useMemo(() => {
    if (!searchQuery) return logLines;
    const query = searchQuery.toLowerCase();
    return logLines.filter(
      line => 
        line.original.toLowerCase().includes(query) || 
        (line.message && line.message.toLowerCase().includes(query))
    );
  }, [logLines, searchQuery]);

  const getLevelColor = (level: string = "") => {
    switch (level) {
      case "info":
        return { color: "#0284c7", bg: "rgba(2, 132, 199, 0.05)" }; // blue-600
      case "warn":
      case "warning":
        return { color: "#b45309", bg: "rgba(180, 83, 9, 0.05)" }; // amber-700
      case "err":
      case "error":
      case "critical":
        return { color: "#dc2626", bg: "rgba(220, 38, 38, 0.05)" }; // red-600
      case "debug":
        return { color: "#7c3aed", bg: "rgba(124, 58, 237, 0.05)" }; // violet-600
      case "trace":
        return { color: "#4b5563", bg: "rgba(75, 85, 99, 0.05)" }; // gray-600
      default:
        return { color: "#1e293b", bg: "transparent" };
    }
  };

  return (
    <Modal
      title={
        <div className="flex items-center gap-2 text-slate-800">
          <FileTextOutlined className="text-indigo-600" />
          <span>系统 spdlog 日志查看器</span>
        </div>
      }
      open={visible}
      onCancel={onClose}
      footer={null}
      width="90vw"
      destroyOnClose
      styles={{
        container: { padding: 0, borderRadius: 8, overflow: 'hidden' },
        header: { backgroundColor: '#f8fafc', borderBottom: '1px solid #e2e8f0', padding: '16px 24px', margin: 0 },
        // content: { backgroundColor: '#ffffff', padding: 0 },
        body: { padding: '16px 24px', backgroundColor: '#ffffff', color: '#1e293b' }
      }}
      closeIcon={<span className="text-slate-500 hover:text-slate-700">✕</span>}
      style={{ top: 40 }}
    >
      <div className="flex flex-col gap-4 h-[75vh]">
        {/* Top Control Bar */}
        <div className="flex flex-wrap justify-between items-center gap-3 border-b border-slate-200 pb-4">
          <Space wrap size="middle">
            <span className="text-sm font-medium text-slate-500">日志文件:</span>
            <Select
              placeholder="选择日志文件"
              value={selectedFile}
              onChange={setSelectedFile}
              className="w-[240px]"
              loading={loadingList}
              options={fileList.map(file => ({ value: file, label: file }))}
            />
          </Space>

          <Space wrap size="middle">
            <Input.Search
              placeholder="过滤日志内容"
              value={searchQuery}
              onChange={e => setSearchQuery(e.target.value)}
              className="w-[200px]"
              allowClear
            />
            <Space.Compact>
              <Button
                icon={<ReloadOutlined className="text-slate-600" />}
                onClick={() => selectedFile && fetchFileContent(selectedFile)}
                disabled={!selectedFile || loading}
                className="bg-slate-50 border border-slate-200 hover:bg-slate-100"
                title="刷新"
              />
              <Button
                icon={<ArrowUpOutlined className="text-slate-600" />}
                onClick={handleScrollToTop}
                className="bg-slate-50 border border-slate-200 hover:bg-slate-100"
                title="置顶"
              />
              <Button
                icon={<ArrowDownOutlined className="text-slate-600" />}
                onClick={handleScrollToBottom}
                className="bg-slate-50 border border-slate-200 hover:bg-slate-100"
                title="置底"
              />
            </Space.Compact>
          </Space>
        </div>

        {/* Log Viewer Screen */}
        <div className="flex-1 relative overflow-hidden rounded border border-slate-200 bg-slate-50">
          {loading ? (
            <div className="absolute inset-0 flex items-center justify-center bg-white/70 z-10">
              <Spin size="large" tip="正在加载日志内容..." />
            </div>
          ) : null}

          <div
            ref={consoleContainerRef}
            className="h-full overflow-y-auto p-3 font-mono text-xs md:text-sm leading-relaxed break-words whitespace-pre-wrap overflow-x-hidden"
          >
            {filteredLines.length === 0 ? (
              <div className="text-slate-400 text-center mt-10">
                {selectedFile ? "暂无匹配日志" : "请先选择一个日志文件"}
              </div>
            ) : (
              filteredLines.map((line, idx) => {
                if (line.time && line.level) {
                  const styleColors = getLevelColor(line.level);
                  return (
                    <div 
                      key={idx} 
                      className="flex flex-col gap-1 mb-2 p-2 border-b border-slate-100 rounded-md"
                      style={{ backgroundColor: styleColors.bg }}
                    >
                      {/* First line: Metadata (date, level) */}
                      <div className="flex flex-wrap items-center gap-2 text-[10px] md:text-xs text-slate-500 select-none">
                        <span className="font-mono">[{line.time}]</span>
                        <span className="font-semibold px-1.5 py-0.2 rounded text-[9px] uppercase border" style={{ color: styleColors.color, borderColor: styleColors.color }}>
                          {line.level}
                        </span>
                      </div>
                      {/* Second line: Actual message text */}
                      <div className="text-xs md:text-sm text-slate-800 font-mono break-all pl-1 leading-relaxed">
                        {line.message}
                      </div>
                    </div>
                  );
                }
                // Plain line (like fallback or multi-line error details)
                return (
                  <div key={idx} className="text-slate-600 pl-1 mb-1 font-mono text-xs md:text-sm break-all">
                    {line.original}
                  </div>
                );
              })
            )}
          </div>
        </div>
      </div>
    </Modal>
  );
};
