import { useState, useEffect, useRef } from "react";
import {
    useAppStore,
    setFglogHandler,
    removeFglogHandler,
    clearLocalStorageHistory,
    getFglogBuffer
} from "../../store/useAppStore";

export interface DataPoint {
    time: number;
    [key: string]: any;
}

export interface SourceConfig {
    id: string;
    name: string;
    topic: string;
    type: "line" | "state" | "log";
    enabled: boolean;
    autoY: boolean;
    locked: boolean;
    color?: string;
}

const COLOR_LIST = ["#2563eb", "#dc2626", "#16a34a", "#eab308", "#8b5cf6", "#d946ef", "#06b6d4"];

export const useVisualizerData = (_: boolean, isLocalMode: boolean, isPlaying: boolean) => {
    const wsStatus = useAppStore((state) => state.wsStatus);
    const [sources, setSources] = useState<SourceConfig[]>([]);
    const [dataCount, setDataCount] = useState<number>(0);

    const dataRef = useRef<DataPoint[]>([]);
    const mergedDataRef = useRef<DataPoint[]>([]);
    const lastTimeRef = useRef<number>(0);
    const sourcesRef = useRef<SourceConfig[]>(sources);

    useEffect(() => {
        sourcesRef.current = sources;
    }, [sources]);

    const defaultSettings = { enabled: false, autoY: false, locked: true };

    // 从历史/文件数据提取源列表
    const extractSourcesFromPoints = (points: DataPoint[]) => {
        const newSourcesMap = new Map<string, SourceConfig>();

        points.forEach((pt) => {
            Object.keys(pt).forEach((key) => {
                if (key === "time" || key === "log") return;

                if (key.endsWith(".x") || key.endsWith(".y") || key.endsWith(".z")) {
                    if (!newSourcesMap.has(key)) {
                        const color = COLOR_LIST[newSourcesMap.size % COLOR_LIST.length];
                        newSourcesMap.set(key, {
                            id: key,
                            name: key.split("/").pop() || key,
                            topic: key,
                            type: "line",
                            ...defaultSettings,
                            color,
                        });
                    }
                } else {
                    if (!newSourcesMap.has(key)) {
                        const val = pt[key];
                        const type = typeof val === "number" ? "line" : "state";
                        const color = COLOR_LIST[newSourcesMap.size % COLOR_LIST.length];
                        newSourcesMap.set(key, {
                            id: key,
                            name: key.split("/").pop() || key,
                            topic: key,
                            type,
                            ...defaultSettings,
                            color,
                        });
                    }
                }
            });

            if (pt.log && !newSourcesMap.has("log")) {
                newSourcesMap.set("log", {
                    id: "log",
                    name: "事件日志",
                    topic: "/drone/diagnostics/log_events",
                    type: "log",
                    ...defaultSettings,
                });
            }
        });

        const nextSources = Array.from(newSourcesMap.values());
        setSources(nextSources);
        sourcesRef.current = nextSources;
    };

    // 初始化历史数据恢复
    useEffect(() => {
        try {
            const countStr = localStorage.getItem("fglog_chunk_count");
            const loadedPoints: DataPoint[] = [];

            if (countStr) {
                const count = parseInt(countStr, 10);
                for (let i = 0; i < count; i++) {
                    const chunkStr = localStorage.getItem(`fglog_chunk_${i}`);
                    if (chunkStr) {
                        const chunkData = JSON.parse(chunkStr) as DataPoint[];
                        loadedPoints.push(...chunkData);
                    }
                }
            }

            const activeBuf = getFglogBuffer();
            loadedPoints.push(...activeBuf);

            mergedDataRef.current = loadedPoints;
            dataRef.current = [...activeBuf];

            if (loadedPoints.length > 0) {
                lastTimeRef.current = loadedPoints[loadedPoints.length - 1].time;
                setDataCount(loadedPoints.length);
                extractSourcesFromPoints(loadedPoints);
            }
        } catch (e) {
            console.error("Failed to load historical chunks from localStorage & store:", e);
        }
    }, []);

    // 接收 WebSocket 实时推送逻辑 (已完整还原原始解包与打标逻辑)
    useEffect(() => {
        if (isLocalMode || wsStatus !== "open") return;

        const handleWsMessage = (data: any) => {
            const fglog = data.fglog ? data.fglog : data;
            if (!fglog || fglog.time === undefined || !fglog.topic || fglog.value === undefined) return;

            const time = fglog.time / 1000.0;
            const { topic, value } = fglog;

            // 1. 动态注册 Source
            setSources((prev) => {
                const exists = prev.some((s) => s.topic === topic || (Array.isArray(value) && s.topic.startsWith(topic + ".")));
                if (exists) return prev;

                const next = [...prev];
                const isVector = Array.isArray(value);
                const color = COLOR_LIST[next.length % COLOR_LIST.length];

                if (isVector) {
                    if (value.length >= 3) {
                        next.push({ id: topic + ".x", name: topic.split("/").pop() + ".x", topic: topic + ".x", type: "line", ...defaultSettings, color });
                        next.push({ id: topic + ".y", name: topic.split("/").pop() + ".y", topic: topic + ".y", type: "line", ...defaultSettings, color: COLOR_LIST[(next.length + 1) % COLOR_LIST.length] });
                        next.push({ id: topic + ".z", name: topic.split("/").pop() + ".z", topic: topic + ".z", type: "line", ...defaultSettings, color: COLOR_LIST[(next.length + 2) % COLOR_LIST.length] });
                    } else if (value.length === 2) {
                        next.push({ id: topic + ".x", name: topic.split("/").pop() + ".x", topic: topic + ".x", type: "line", ...defaultSettings, color });
                        next.push({ id: topic + ".y", name: topic.split("/").pop() + ".y", topic: topic + ".y", type: "line", ...defaultSettings, color: COLOR_LIST[(next.length + 1) % COLOR_LIST.length] });
                    }
                } else {
                    const type = fglog.type === "value" ? "line" : (fglog.type === "log" ? "log" : "state");
                    next.push({ id: topic, name: topic.split("/").pop() || topic, topic, type, ...defaultSettings, color });
                }

                sourcesRef.current = next;
                return next;
            });

            // 2. 构建 DataPoint (还原 Vector 展开与 hardcoded ID 匹配逻辑)
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
                let id = topic;
                const matchedHardcoded = sourcesRef.current.find((s) => s.topic === topic);
                if (matchedHardcoded) {
                    id = matchedHardcoded.id;
                }

                if (fglog.type === "log") {
                    dataPoint.log = value;
                } else {
                    dataPoint[id] = value;
                }
            }

            dataRef.current.push(dataPoint);
            mergedDataRef.current.push(dataPoint);

            if (dataRef.current.length >= 2000) {
                dataRef.current = dataRef.current.slice(1000);
            }

            // 根据 isPlaying 判定是否将时间轴向前推送
            if (isPlaying) {
                lastTimeRef.current = time;
            }

            setDataCount(mergedDataRef.current.length);
        };

        setFglogHandler(handleWsMessage);
        return () => removeFglogHandler();
    }, [isLocalMode, isPlaying, wsStatus]);

    const clearData = () => {
        dataRef.current = [];
        mergedDataRef.current = [];
        lastTimeRef.current = 0;
        setDataCount(0);
        clearLocalStorageHistory();
    };

    return {
        sources,
        setSources,
        sourcesRef,
        mergedDataRef,
        lastTimeRef,
        dataCount,
        clearData,
    };
};