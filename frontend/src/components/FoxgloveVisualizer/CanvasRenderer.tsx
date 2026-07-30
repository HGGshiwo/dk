import React, { useState, useEffect, useRef } from "react";
import type { SourceConfig, DataPoint } from "./useVisualizerData";

interface Props {
    visible: boolean;
    sources: SourceConfig[];
    sourcesRef: React.MutableRefObject<SourceConfig[]>;
    mergedDataRef: React.MutableRefObject<DataPoint[]>;
    lastTimeRef: React.MutableRefObject<number>;
    dragOffset: number;
    setDragOffset: React.Dispatch<React.SetStateAction<number>>;
    timeWindow: number;
    setTimeWindow: React.Dispatch<React.SetStateAction<number>>;
    setIsPlaying: (playing: boolean) => void;
    zoomFactors: Record<string, number>;
    setZoomFactors: React.Dispatch<React.SetStateAction<Record<string, number>>>;
    setSources: React.Dispatch<React.SetStateAction<SourceConfig[]>>;
}

const STATE_COLORS: Record<string, string> = {
    DISARM: "#94a3b8", ARMED: "#3b82f6", TAKEOFF: "#8b5cf6", GUIDED: "#10b981", LANDING: "#f59e0b",
    FAILSAFE: "#ef4444", MANUAL: "#f43f5e", ALT_HOLD: "#14b8a6", LOITER: "#06b6d4", AUTO: "#22c55e", LAND: "#d97706"
};

const getStateColor = (val: any) => {
    if (val === undefined || val === null) return "#cbd5e1";
    const valStr = String(val).toUpperCase();
    if (STATE_COLORS[valStr]) return STATE_COLORS[valStr];
    if (val === true || valStr === "TRUE" || valStr === "ON" || valStr === "ARMED" || valStr === "ARM") return "#10b981";
    if (val === false || valStr === "FALSE" || valStr === "OFF" || valStr === "DISARMED" || valStr === "DISARM") return "#ef4444";

    let hash = 0;
    for (let i = 0; i < valStr.length; i++) {
        hash = valStr.charCodeAt(i) + ((hash << 5) - hash);
    }
    const colors = ["#3b82f6", "#ef4444", "#10b981", "#f59e0b", "#8b5cf6", "#ec4899", "#14b8a6", "#6366f1"];
    return colors[Math.abs(hash) % colors.length];
};

export const CanvasRenderer: React.FC<Props> = ({
    visible,
    sourcesRef,
    mergedDataRef,
    dragOffset,
    setDragOffset,
    timeWindow,
    setTimeWindow,
    setIsPlaying,
    zoomFactors,
    setZoomFactors,
    setSources,
}) => {
    const [canvasElement, setCanvasElement] = useState<HTMLCanvasElement | null>(null);
    const canvasRef = useRef<HTMLCanvasElement | null>(null);
    const requestRef = useRef<number | null>(null);

    const hoverXRef = useRef<number | null>(null);
    const hoverYRef = useRef<number | null>(null);

    const timeWindowRef = useRef<number>(timeWindow);
    const dragOffsetRef = useRef<number>(dragOffset);
    const zoomFactorsRef = useRef<Record<string, number>>(zoomFactors);

    useEffect(() => { timeWindowRef.current = timeWindow; }, [timeWindow]);
    useEffect(() => { dragOffsetRef.current = dragOffset; }, [dragOffset]);
    useEffect(() => { zoomFactorsRef.current = zoomFactors; }, [zoomFactors]);

    // 手势/拖拽控制变量
    const isDraggingRef = useRef<boolean>(false);
    const isTouchYAxisRef = useRef<boolean>(false);
    const isTouchXAxisRef = useRef<boolean>(false);
    const touchStartClientXRef = useRef<number>(0);
    const touchStartClientYRef = useRef<number>(0);
    const touchStartDragOffsetRef = useRef<number>(0);
    const touchStartTimeWindowRef = useRef<number>(30);
    const touchStartZoomFactorsRef = useRef<Record<string, number>>({});

    const setCanvasRef = (node: HTMLCanvasElement | null) => {
        canvasRef.current = node;
        setCanvasElement(node);
    };

    // Canvas 渲染主循环与 Wheel 监听
    useEffect(() => {
        if (!visible || !canvasElement) return;
        const canvas = canvasElement;
        const ctx = canvas.getContext("2d");
        if (!ctx) return;

        const handleWheel = (e: WheelEvent) => {
            e.preventDefault();
            const rect = canvas.getBoundingClientRect();
            const hX = hoverXRef.current;
            const hY = hoverYRef.current;
            const isYAxisArea = hX !== null && hX < 45;
            const isXAxisArea = hX !== null && hY !== null && hX >= 45 && hY >= rect.height - 30;

            if (isYAxisArea) {
                const zoomFactor = e.deltaY < 0 ? 0.9 : 1.1;
                const nextSources = sourcesRef.current.map((s) => (s.type === "line" && s.enabled && !s.locked && s.autoY ? { ...s, autoY: false } : s));
                sourcesRef.current = nextSources;
                setSources(nextSources);

                const nextFactors = { ...zoomFactorsRef.current };
                sourcesRef.current.forEach((s) => {
                    if (s.type === "line" && s.enabled && !s.locked) {
                        const currentFactor = nextFactors[s.id] || 1.0;
                        nextFactors[s.id] = Math.max(0.1, Math.min(10.0, currentFactor / zoomFactor));
                    }
                });
                zoomFactorsRef.current = nextFactors;
                setZoomFactors(nextFactors);
            } else if (isXAxisArea || e.ctrlKey) {
                const zoomFactor = e.deltaY < 0 ? 0.9 : 1.1;
                setTimeWindow((prev) => {
                    const val = Math.max(2, Math.min(180, prev * zoomFactor));
                    timeWindowRef.current = val;
                    return val;
                });
            } else {
                const panSpeed = timeWindowRef.current * 0.05;
                const deltaOffset = e.deltaY > 0 ? -panSpeed : panSpeed;
                setDragOffset((prev) => {
                    const next = Math.max(0, prev + deltaOffset);
                    if (next > 0) setIsPlaying(false);
                    dragOffsetRef.current = next;
                    return next;
                });
            }
        };

        canvas.addEventListener("wheel", handleWheel, { passive: false });

        const resizeCanvas = () => {
            const rect = canvas.parentElement?.getBoundingClientRect();
            if (rect) {
                canvas.width = (rect.width || 800) * window.devicePixelRatio;
                canvas.height = (rect.height || 400) * window.devicePixelRatio;
                canvas.style.width = "100%";
                canvas.style.height = "100%";
            }
        };

        const resizeObserver = new ResizeObserver(() => resizeCanvas());
        if (canvas.parentElement) resizeObserver.observe(canvas.parentElement);

        // 帧渲染逻辑
        const draw = () => {
            if (!ctx || !canvas) return;
            if (canvas.width === 0 || canvas.height === 0) resizeCanvas();

            const width = canvas.width;
            const height = canvas.height;
            const ratio = window.devicePixelRatio;

            ctx.clearRect(0, 0, width, height);
            ctx.save();
            ctx.scale(ratio, ratio);

            const w = width / ratio;
            const h = height / ratio;
            const paddingLeft = 45;
            const paddingRight = 15;
            const paddingTop = 8;
            const paddingBottom = 30;
            const chartWidth = w - paddingLeft - paddingRight;
            const chartHeight = h - paddingTop - paddingBottom;
            const data = mergedDataRef.current;

            const getLatestValue = (targetTime: number, key: string): number | undefined => {
                for (let i = data.length - 1; i >= 0; i--) {
                    if (data[i].time <= targetTime) {
                        const val = data[i][key];
                        if (typeof val === "number" && !isNaN(val)) return val;
                    }
                }
                return undefined;
            };

            const getLatestStateValue = (targetTime: number, key: string): any => {
                for (let i = data.length - 1; i >= 0; i--) {
                    if (data[i].time <= targetTime && data[i][key] !== undefined) return data[i][key];
                }
                return undefined;
            };

            if (data.length === 0) {
                ctx.fillStyle = "#94a3b8";
                ctx.font = "13px sans-serif";
                ctx.textAlign = "center";
                ctx.fillText("等待数据输入...", w / 2, h / 2);
                ctx.restore();
                requestRef.current = requestAnimationFrame(draw);
                return;
            }

            if (data.length > 0) {
                let lastAbs = data[0].time;
                let lastComp = 0;
                data[0].compressedTime = 0;
                for (let i = 1; i < data.length; i++) {
                    const currentAbs = data[i].time;
                    let dt = currentAbs - lastAbs;
                    if (dt < 0) {
                        dt = 0;
                    } else if (dt > 10) {
                        dt = 2;
                    }
                    const currentComp = lastComp + dt;
                    data[i].compressedTime = currentComp;
                    lastAbs = currentAbs;
                    lastComp = currentComp;
                }
            }

            const formatAbsoluteTime = (timeSec: number) => {
                const d = new Date(timeSec * 1000);
                const hrs = String(d.getHours()).padStart(2, "0");
                const mins = String(d.getMinutes()).padStart(2, "0");
                const secs = String(d.getSeconds()).padStart(2, "0");
                const tenths = Math.floor(d.getMilliseconds() / 100);
                return `${hrs}:${mins}:${secs}.${tenths}`;
            };

            const maxTimeComp = data.length > 0 ? data[data.length - 1].compressedTime! : 0;
            const tMaxComp = maxTimeComp - dragOffsetRef.current;
            const tMinComp = tMaxComp - timeWindowRef.current;

            const tToX = (compTime: number) => paddingLeft + ((compTime - tMinComp) / timeWindowRef.current) * chartWidth;
            const xToT = (x: number) => tMinComp + ((x - paddingLeft) / chartWidth) * timeWindowRef.current;

            const lineSources = sourcesRef.current.filter((s) => s.type === "line" && s.enabled);
            const stateSources = sourcesRef.current.filter((s) => s.type === "state" && s.enabled);
            const logSource = sourcesRef.current.find((s) => s.type === "log" && s.enabled);

            const lineActive = lineSources.length > 0;
            const stateCount = stateSources.length;
            const logActive = !!logSource;

            const thinTrackHeight = 10;
            const trackGap = 3;
            const othersHeight = stateCount * thinTrackHeight + (logActive ? thinTrackHeight : 0) + (stateCount + (logActive ? 1 : 0)) * trackGap;
            const lineTrackHeight = Math.max(50, chartHeight - othersHeight);
            let currentY = paddingTop;

            const hX = hoverXRef.current;
            const hY = hoverYRef.current;
            const isHoveringYAxis = hX !== null && hX < paddingLeft;
            const isHoveringXAxis = hX !== null && hY !== null && hX >= paddingLeft && hY >= h - paddingBottom;

            // 画背景与边框
            const drawBorderAndBg = (yStart: number, yHeight: number) => {
                ctx.fillStyle = "#f8fafc";
                ctx.fillRect(paddingLeft, yStart, chartWidth, yHeight);
                ctx.strokeStyle = "#cbd5e1";
                ctx.lineWidth = 0.8;
                ctx.strokeRect(paddingLeft, yStart, chartWidth, yHeight);
            };

            // 1. 折线图绘制
            if (lineActive) {
                if (isHoveringYAxis) {
                    ctx.fillStyle = "rgba(59, 130, 246, 0.07)";
                    ctx.fillRect(0, currentY, paddingLeft, lineTrackHeight);
                }
                drawBorderAndBg(currentY, lineTrackHeight);

                ctx.strokeStyle = "#e2e8f0";
                ctx.lineWidth = 0.5;
                for (let i = 1; i < 4; i++) {
                    const y = currentY + (i / 4) * lineTrackHeight;
                    ctx.beginPath();
                    ctx.moveTo(paddingLeft, y);
                    ctx.lineTo(w - paddingRight, y);
                    ctx.stroke();
                }

                ctx.save();
                const clipPath = new Path2D();
                clipPath.rect(paddingLeft, currentY, chartWidth, lineTrackHeight);
                ctx.clip(clipPath);

                lineSources.forEach((src) => {
                    const key = src.id;
                    let localMin = Infinity;
                    let localMax = -Infinity;
                    let hasLocalPoints = false;

                    for (const pt of data) {
                        if (pt.compressedTime! >= tMinComp && pt.compressedTime! <= tMaxComp) {
                            const val = pt[key];
                            if (typeof val === "number" && !isNaN(val)) {
                                hasLocalPoints = true;
                                localMin = Math.min(localMin, val);
                                localMax = Math.max(localMax, val);
                            }
                        }
                    }
                    if (!hasLocalPoints) { localMin = -3; localMax = 3; }

                    const baseVal = Math.max(0.1, Math.abs(localMin), Math.abs(localMax)) * 1.08;
                    let activeMin = -baseVal;
                    let activeMax = baseVal;

                    if (!src.autoY) {
                        const factor = zoomFactorsRef.current[src.id] || 1.0;
                        activeMin = -baseVal / factor;
                        activeMax = baseVal / factor;
                    }

                    const localValToY = (v: number) => currentY + lineTrackHeight - ((v - activeMin) / (activeMax - activeMin)) * lineTrackHeight;

                    ctx.strokeStyle = src.color || "#000";
                    ctx.lineWidth = 1.6;
                    ctx.beginPath();
                    let first = true;
                    for (const pt of data) {
                        if (pt.compressedTime! < tMinComp - 2) continue;
                        if (pt.compressedTime! > tMaxComp + 2) break;
                        const val = pt[key];
                        if (val === undefined || val === null || typeof val !== "number" || isNaN(val)) continue;

                        const x = tToX(pt.compressedTime!);
                        const y = localValToY(val);
                        if (first) { ctx.moveTo(x, y); first = false; } else { ctx.lineTo(x, y); }
                    }
                    ctx.stroke();
                });
                ctx.restore();

                // 刻度显示
                const primaryRefId = sourcesRef.current.find((s) => s.type === "line" && s.enabled && !s.locked)?.id || lineSources[0]?.id;
                if (primaryRefId) {
                    const refSrc = sourcesRef.current.find((s) => s.id === primaryRefId)!;
                    let localMin = Infinity;
                    let localMax = -Infinity;
                    data.forEach((pt) => {
                        if (pt.compressedTime! >= tMinComp && pt.compressedTime! <= tMaxComp) {
                            const val = pt[primaryRefId];
                            if (typeof val === "number" && !isNaN(val)) {
                                localMin = Math.min(localMin, val);
                                localMax = Math.max(localMax, val);
                            }
                        }
                    });
                    if (localMin === Infinity) { localMin = -3; localMax = 3; }
                    const baseVal = Math.max(0.1, Math.abs(localMin), Math.abs(localMax)) * 1.08;
                    const factor = !refSrc.autoY ? zoomFactorsRef.current[primaryRefId] || 1.0 : 1.0;
                    const activeMax = baseVal / factor;
                    const activeMin = -baseVal / factor;

                    ctx.fillStyle = isHoveringYAxis ? "#2563eb" : refSrc.color || "#64748b";
                    ctx.font = "bold 11px monospace";
                    ctx.textAlign = "right";
                    ctx.textBaseline = "middle";
                    ctx.fillText(activeMax.toFixed(1), paddingLeft - 5, currentY);
                    ctx.fillText(((activeMax + activeMin) / 2).toFixed(1), paddingLeft - 5, currentY + lineTrackHeight / 2);
                    ctx.fillText(activeMin.toFixed(1), paddingLeft - 5, currentY + lineTrackHeight);
                }

                currentY += lineTrackHeight + trackGap;
            }

            // 2. 状态轨道绘制
            stateSources.forEach((src) => {
                drawBorderAndBg(currentY, thinTrackHeight);
                ctx.save();
                const clipPath = new Path2D();
                clipPath.rect(paddingLeft, currentY, chartWidth, thinTrackHeight);
                ctx.clip(clipPath);

                const key = src.id;
                const statePoints = data.filter((pt) => pt[key] !== undefined && pt[key] !== null);

                if (statePoints.length > 0) {
                    for (let i = 0; i < statePoints.length; i++) {
                        const pt = statePoints[i];
                        const nextPt = statePoints[i + 1];
                        const startT = pt.compressedTime!;
                        const endT = nextPt ? nextPt.compressedTime! : tMaxComp;
                        const val = pt[key];
                        const xStart = tToX(startT);
                        const xEnd = tToX(endT);

                        if (xEnd > xStart) {
                            ctx.fillStyle = getStateColor(val);
                            ctx.fillRect(xStart, currentY + 0.5, Math.max(xEnd - xStart, 1), thinTrackHeight - 1);
                        }
                    }
                }
                ctx.restore();

                ctx.fillStyle = "#64748b";
                ctx.font = "bold 9px sans-serif";
                ctx.textAlign = "right";
                ctx.textBaseline = "middle";
                ctx.fillText(src.name.substring(0, 2), paddingLeft - 6, currentY + thinTrackHeight / 2);

                currentY += thinTrackHeight + trackGap;
            });

            // 3. 日志点绘制
            let logYStart = 0;
            if (logActive) {
                logYStart = currentY;
                drawBorderAndBg(logYStart, thinTrackHeight);
                ctx.save();
                const clipPath = new Path2D();
                clipPath.rect(paddingLeft, logYStart, chartWidth, thinTrackHeight);
                ctx.clip(clipPath);

                for (const pt of data) {
                    if (pt.compressedTime! < tMinComp) continue;
                    if (pt.compressedTime! > tMaxComp) break;
                    if (pt.log) {
                        const x = tToX(pt.compressedTime!);
                        ctx.fillStyle = "#7c3aed";
                        ctx.beginPath();
                        ctx.arc(x, logYStart + thinTrackHeight / 2, 2.5, 0, Math.PI * 2);
                        ctx.fill();
                    }
                }
                ctx.restore();
                ctx.fillStyle = "#64748b";
                ctx.font = "bold 9px sans-serif";
                ctx.textAlign = "right";
                ctx.textBaseline = "middle";
                ctx.fillText("日志", paddingLeft - 6, logYStart + thinTrackHeight / 2);
            }

            // X 轴底部时间网格与文本
            if (isHoveringXAxis) {
                ctx.fillStyle = "rgba(59, 130, 246, 0.07)";
                ctx.fillRect(paddingLeft, h - paddingBottom, chartWidth, paddingBottom);
            }

            ctx.strokeStyle = "#e2e8f0";
            ctx.lineWidth = 0.5;
            for (let i = 0; i <= 5; i++) {
                const compT = tMinComp + (i / 5) * timeWindowRef.current;
                const x = paddingLeft + (i / 5) * chartWidth;
                
                let closestPt = data[0];
                if (data.length > 0) {
                    let minDist = Math.abs(data[0].compressedTime! - compT);
                    for (const pt of data) {
                        const dist = Math.abs(pt.compressedTime! - compT);
                        if (dist < minDist) {
                            minDist = dist;
                            closestPt = pt;
                        }
                    }
                }

                if (x >= paddingLeft && x <= w - paddingRight) {
                    ctx.beginPath();
                    ctx.moveTo(x, paddingTop);
                    ctx.lineTo(x, h - paddingBottom);
                    ctx.stroke();

                    ctx.fillStyle = isHoveringXAxis ? "#2563eb" : "#475569";
                    ctx.font = "12px monospace";
                    ctx.textAlign = "center";
                    ctx.textBaseline = "top";
                    ctx.fillText(formatAbsoluteTime(closestPt.time), x, h - paddingBottom + 5);
                }
            }

            // Tooltip 悬浮框绘制
            if (hX !== null && hX >= paddingLeft && hX <= w - paddingRight && !isHoveringYAxis && !isHoveringXAxis) {
                const activeCompT = xToT(hX);
                ctx.strokeStyle = "rgba(0, 0, 0, 0.4)";
                ctx.lineWidth = 1;
                ctx.setLineDash([3, 3]);
                ctx.beginPath();
                ctx.moveTo(hX, paddingTop);
                ctx.lineTo(hX, h - paddingBottom);
                ctx.stroke();
                ctx.setLineDash([]);

                let activePoint: DataPoint | null = null;
                let minDist = Infinity;
                for (const pt of data) {
                    const dist = Math.abs(pt.compressedTime! - activeCompT);
                    if (dist < minDist) { minDist = dist; activePoint = pt; }
                }

                if (activePoint) {
                    const tooltipWidth = 180;
                    const tooltipHeight = (1 + lineSources.length + stateSources.length) * 15 + 10;
                    let tooltipX = hX + 10;
                    if (tooltipX + tooltipWidth > w) tooltipX = hX - tooltipWidth - 10;
                    const tooltipY = Math.max(paddingTop + 5, h / 2 - tooltipHeight / 2);

                    ctx.fillStyle = "rgba(255, 255, 255, 0.96)";
                    ctx.fillRect(tooltipX, tooltipY, tooltipWidth, tooltipHeight);
                    ctx.strokeStyle = "#94a3b8";
                    ctx.lineWidth = 0.8;
                    ctx.strokeRect(tooltipX, tooltipY, tooltipWidth, tooltipHeight);

                    ctx.font = "bold 11px monospace";
                    ctx.fillStyle = "#1e293b";
                    ctx.textAlign = "left";
                    ctx.textBaseline = "top";
                    let textY = tooltipY + 6;
                    ctx.fillText(`时间: ${formatAbsoluteTime(activePoint.time)}`, tooltipX + 8, textY);
                    textY += 15;

                    ctx.font = "11px monospace";
                    lineSources.forEach((src) => {
                        const val = getLatestValue(activePoint!.time, src.id);
                        ctx.fillStyle = src.color || "#000";
                        ctx.fillText(`${src.name}: ${val !== undefined ? val.toFixed(2) : "N/A"}`, tooltipX + 8, textY);
                        textY += 15;
                    });

                    stateSources.forEach((src) => {
                        const stateVal = getLatestStateValue(activePoint!.time, src.id);
                        if (stateVal !== undefined) {
                            ctx.fillStyle = "#475569";
                            ctx.fillText(`${src.name.substring(0, 5)}: ${stateVal}`, tooltipX + 8, textY);
                            textY += 15;
                        }
                    });
                }
            }

            ctx.restore();
            requestRef.current = requestAnimationFrame(draw);
        };

        draw();
        return () => {
            canvas.removeEventListener("wheel", handleWheel);
            resizeObserver.disconnect();
            if (requestRef.current) cancelAnimationFrame(requestRef.current);
        };
    }, [visible, canvasElement]);

    // 鼠标拖拽事件处理
    const handleMouseDown = (e: React.MouseEvent<HTMLCanvasElement>) => {
        if (!canvasElement) return;
        const rect = canvasElement.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const y = e.clientY - rect.top;

        isDraggingRef.current = true;
        isTouchYAxisRef.current = x < 45;
        isTouchXAxisRef.current = x >= 45 && y >= rect.height - 30;

        touchStartClientXRef.current = e.clientX;
        touchStartClientYRef.current = e.clientY;
        touchStartDragOffsetRef.current = dragOffsetRef.current;
        touchStartTimeWindowRef.current = timeWindowRef.current;

        const currentFactors: Record<string, number> = {};
        sourcesRef.current.forEach((s) => { currentFactors[s.id] = zoomFactorsRef.current[s.id] || 1.0; });
        touchStartZoomFactorsRef.current = currentFactors;
    };

    const handleMouseMove = (e: React.MouseEvent<HTMLCanvasElement>) => {
        if (!canvasElement) return;
        const rect = canvasElement.getBoundingClientRect();
        hoverXRef.current = e.clientX - rect.left;
        hoverYRef.current = e.clientY - rect.top;

        if (!isDraggingRef.current) return;

        if (isTouchYAxisRef.current) {
            const deltaY = touchStartClientYRef.current - e.clientY;
            const zoomRatio = Math.max(0.1, Math.min(10.0, 1.0 + deltaY / 150));
            const nextFactors = { ...zoomFactorsRef.current };
            sourcesRef.current.forEach((s) => {
                if (s.type === "line" && s.enabled && !s.locked) {
                    const startFactor = touchStartZoomFactorsRef.current[s.id] || 1.0;
                    nextFactors[s.id] = Math.max(0.1, Math.min(10.0, startFactor * zoomRatio));
                }
            });
            zoomFactorsRef.current = nextFactors;
            setZoomFactors(nextFactors);
        } else if (isTouchXAxisRef.current) {
            const deltaX = e.clientX - touchStartClientXRef.current;
            const zoomRatio = Math.max(0.1, Math.min(10.0, 1.0 + deltaX / 150));
            setTimeWindow(() => {
                const val = Math.max(2, Math.min(180, touchStartTimeWindowRef.current / zoomRatio));
                timeWindowRef.current = val;
                return val;
            });
        } else {
            const deltaX = e.clientX - touchStartClientXRef.current;
            const chartWidth = rect.width - 60;
            const timeDelta = (deltaX / chartWidth) * timeWindowRef.current;
            setDragOffset(() => {
                const next = Math.max(0, touchStartDragOffsetRef.current - timeDelta);
                if (next > 0) setIsPlaying(false);
                dragOffsetRef.current = next;
                return next;
            });
        }
    };

    return (
        <div className="flex-1 border border-slate-200 rounded overflow-hidden relative bg-white min-h-[220px]">
            <canvas
                ref={setCanvasRef}
                onMouseDown={handleMouseDown}
                onMouseMove={handleMouseMove}
                onMouseUp={() => { isDraggingRef.current = false; }}
                onMouseLeave={() => { isDraggingRef.current = false; hoverXRef.current = null; hoverYRef.current = null; }}
                className="absolute top-0 left-0 cursor-ew-resize"
                style={{ touchAction: "none" }}
            />
        </div>
    );
};