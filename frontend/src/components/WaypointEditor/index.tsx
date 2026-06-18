import React, { useRef, useEffect, useState, useCallback } from "react";
import {
  Button,
  InputNumber,
  Switch,
  Table,
  Space,
  message,
  Radio,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import "./WaypointEditor.css";
import { useAppStore } from "../../store/useAppStore";
import { httpRequest } from "../../utils";
import { useOrigin } from "./hooks";

interface Point {
  x: number;
  y: number;
}

interface Waypoint {
  id: string;
  lat: number;
  lon: number;
  height: number;
}

export interface WaypointData {
  lat: number;
  lon: number;
  alt: number;
}

interface ViewState {
  offsetX: number;
  offsetY: number;
  scale: number;
}

export interface WaypointEditorProps {
  waypointSubmitUrl?: string;
  followSubmitUrl?: string;
  onEditModeChange?: (isEditing: boolean) => void; // 新增回调
}

const HIT_TOLERANCE = 10;
const DRAG_THRESHOLD = 5;
const FOLLOW_INTERVAL_MS = 500;
const GRID_STEP = 10;
const METERS_PER_DEGREE_LAT = 111320;
const DEFAULT_HEIGHT = 10;

const METERS_PER_DEGREE_LON = (lat: number) =>
  111320 * Math.cos((lat * Math.PI) / 180);

const WaypointEditor: React.FC<WaypointEditorProps> = ({
  waypointSubmitUrl = "/set_waypoint",
  followSubmitUrl = "/set_posvel",
  onEditModeChange,
}) => {
  const [mode, setMode] = useState<"waypoint" | "follow">("waypoint");
  const [showWaypointPanel, setShowWaypointPanel] = useState(false); // 控制航点面板显示

  // 当编辑模式改变时通知父组件
  useEffect(() => {
    onEditModeChange?.(showWaypointPanel);
  }, [showWaypointPanel, onEditModeChange]);

  const [isEditing, setIsEditing] = useState(false);
  const mission_data = useAppStore((state) => state.stateData.mission_data);
  const wp_idx = useAppStore((state) => state.stateData.wp_idx);

  const [waypoints, setWaypoints] = useState<Waypoint[]>([]);
  const [isLoading, setIsLoading] = useState<boolean>(true);
  const needRedrawRef = useRef(false);
  const requestRedraw = useCallback(() => {
    needRedrawRef.current = true;
  }, []);

  const cb = useCallback(() => {
    setIsLoading(false);
    requestRedraw();
  }, [requestRedraw]);

  const originRef = useOrigin(cb);

  const historyRef = useRef<{
    undoStack: Waypoint[][];
    redoStack: Waypoint[][];
  }>({ undoStack: [], redoStack: [] });

  const pushToHistory = useCallback((newWaypoints: Waypoint[]) => {
    historyRef.current.undoStack.push(newWaypoints);
    historyRef.current.redoStack = [];
  }, []);

  const undo = useCallback(() => {
    const { undoStack, redoStack } = historyRef.current;
    if (undoStack.length === 0) return;
    const current = undoStack.pop()!;
    redoStack.push(current);
    const previous = undoStack[undoStack.length - 1];
    if (previous) {
      setWaypoints(previous);
    } else {
      setWaypoints([]);
    }
  }, []);

  const redo = useCallback(() => {
    const { undoStack, redoStack } = historyRef.current;
    if (redoStack.length === 0) return;
    const next = redoStack.pop()!;
    undoStack.push(next);
    setWaypoints(next);
  }, []);

  // 从后端数据派生显示的航点
  const backendWaypoints = React.useMemo<Waypoint[]>(() => {
    return ((mission_data as WaypointData[]) || []).map((wp, idx) => ({
      id: `backend-wp-${idx}`,
      lat: wp.lat,
      lon: wp.lon,
      height: wp.alt,
    }));
  }, [mission_data]);

  // 监听面板关闭：如果关闭面板，则强制退出编辑状态，丢弃未提交的修改
  useEffect(() => {
    if (!showWaypointPanel) {
      setIsEditing(false);
    }
  }, [showWaypointPanel]);

  const handleCancelEdit = useCallback(() => {
    setIsEditing(false);
    setShowWaypointPanel(false);
    // 退出编辑状态后，下面的 useEffect 会自动将数据恢复为后端的 mission_data
  }, []);

  const DEFAULT_SCALE = 10;
  const viewRef = useRef<ViewState>({
    offsetX: 0,
    offsetY: 0,
    scale: DEFAULT_SCALE,
  });

  const interactionRef = useRef<{
    type: "none" | "dragging-waypoint" | "panning";
    waypointId?: string;
    startOffset?: { dx: number; dy: number };
    startWorld?: Point;
    startCanvas?: { x: number; y: number };
  }>({ type: "none" });

  const waypointsRef = useRef(waypoints);
  useEffect(() => {
    waypointsRef.current = waypoints;
  }, [waypoints]);

  // 单向数据流：非编辑状态下，强制与后端 mission_data 同步
  useEffect(() => {
    if (!isEditing) {
      const newWaypoints = ((mission_data as WaypointData[]) || []).map(
        (wp, idx) => ({
          id: `wp-${Date.now()}-${idx}-${Math.random()}`,
          lat: wp.lat,
          lon: wp.lon,
          height: wp.alt,
        }),
      );
      setWaypoints(newWaypoints);
      // 清空历史记录栈并设为同步后的初始状态
      historyRef.current = { undoStack: [newWaypoints], redoStack: [] };
    }
  }, [mission_data, isEditing]);

  // 决定当前显示的数据流
  const displayWaypoints = isEditing ? waypoints : backendWaypoints;

  const [followState, setFollowState] = useState({
    isDrawing: false,
    startPoint: null as Point | null,
    currentMousePoint: null as Point | null,
    fixedHeading: false,
    followHeight: DEFAULT_HEIGHT,
    followSpeed: 2,
    isFollowing: false,
  });

  const canvasRef = useRef<HTMLCanvasElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  const followIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const drawCanvasRef = useRef<() => void>(() => {});

  const latLonToWorld = useCallback(
    (lat: number, lon: number): Point => {
      const origin = originRef.current;
      if (!origin) return { x: 0, y: 0 };
      return {
        x: (lon - origin.lon) * METERS_PER_DEGREE_LON(origin.lat),
        y: (lat - origin.lat) * METERS_PER_DEGREE_LAT,
      };
    },
    [originRef],
  );

  const worldToLatLon = useCallback(
    (worldX: number, worldY: number): { lat: number; lon: number } => {
      const origin = originRef.current;
      if (!origin) return { lat: 0, lon: 0 };
      return {
        lon: worldX / METERS_PER_DEGREE_LON(origin.lat) + origin.lon,
        lat: worldY / METERS_PER_DEGREE_LAT + origin.lat,
      };
    },
    [originRef],
  );

  const worldToCanvas = (worldX: number, worldY: number) => {
    const canvas = canvasRef.current;
    if (!canvas) return { canvasX: 0, canvasY: 0 };
    const rect = canvas.getBoundingClientRect();
    const centerX = rect.width / 2;
    const centerY = rect.height / 2;
    const { scale, offsetX, offsetY } = viewRef.current;
    return {
      canvasX: centerX + worldX * scale + offsetX,
      canvasY: centerY - worldY * scale + offsetY,
    };
  };

  const canvasToWorld = (canvasX: number, canvasY: number): Point => {
    const canvas = canvasRef.current;
    if (!canvas) return { x: 0, y: 0 };
    const rect = canvas.getBoundingClientRect();
    const centerX = rect.width / 2;
    const centerY = rect.height / 2;
    const { scale, offsetX, offsetY } = viewRef.current;

    return {
      x: (canvasX - centerX - offsetX) / scale,
      y: -(canvasY - centerY - offsetY) / scale,
    };
  };

  const drawGrid = useCallback(
    (ctx: CanvasRenderingContext2D, canvas: HTMLCanvasElement) => {
      const topLeft = canvasToWorld(0, 0);
      const topRight = canvasToWorld(canvas.width, 0);
      const bottomLeft = canvasToWorld(0, canvas.height);
      const bottomRight = canvasToWorld(canvas.width, canvas.height);

      const minWorldX = Math.min(
        topLeft.x,
        topRight.x,
        bottomLeft.x,
        bottomRight.x,
      );
      const maxWorldX = Math.max(
        topLeft.x,
        topRight.x,
        bottomLeft.x,
        bottomRight.x,
      );
      const minWorldY = Math.min(
        topLeft.y,
        topRight.y,
        bottomLeft.y,
        bottomRight.y,
      );
      const maxWorldY = Math.max(
        topLeft.y,
        topRight.y,
        bottomLeft.y,
        bottomRight.y,
      );

      ctx.strokeStyle = "#ccc";
      ctx.lineWidth = 0.5;
      ctx.beginPath();

      const startLon = Math.floor(minWorldX / GRID_STEP) * GRID_STEP;
      for (let lon = startLon; lon <= maxWorldX; lon += GRID_STEP) {
        const { canvasX } = worldToCanvas(lon, 0);
        if (canvasX >= 0 && canvasX <= canvas.width) {
          ctx.moveTo(canvasX, 0);
          ctx.lineTo(canvasX, canvas.height);
        }
      }

      const startLat = Math.floor(minWorldY / GRID_STEP) * GRID_STEP;
      for (let lat = startLat; lat <= maxWorldY; lat += GRID_STEP) {
        const { canvasY } = worldToCanvas(0, lat);
        if (canvasY >= 0 && canvasY <= canvas.height) {
          ctx.moveTo(0, canvasY);
          ctx.lineTo(canvas.width, canvasY);
        }
      }
      ctx.stroke();
    },
    [],
  );

  const drawCanvas = useCallback(() => {
    const lon = useAppStore.getState().stateData.lon || 0;
    const lat = useAppStore.getState().stateData.lat || 0;
    const yaw = useAppStore.getState().stateData.yaw || 0;

    const currentPoint = {
      x:
        ((lon as number) - originRef.current.lon) *
        METERS_PER_DEGREE_LON(originRef.current.lat),
      y: ((lat as number) - originRef.current.lat) * METERS_PER_DEGREE_LAT,
      yaw: yaw as number,
    };

    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const rect = canvas.getBoundingClientRect();
    canvas.width = rect.width;
    canvas.height = rect.height;
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    ctx.fillStyle = "#f0f0f0";
    ctx.fillRect(0, 0, canvas.width, canvas.height);

    if (isLoading) {
      ctx.fillStyle = "rgba(240, 240, 240, 0.9)";
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.fillStyle = "#666";
      ctx.font = "16px Arial";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText("等待定位数据...", canvas.width / 2, canvas.height / 2);
      return;
    }

    drawGrid(ctx, canvas);

    ctx.strokeStyle = "#333";
    ctx.lineWidth = 2;
    ctx.beginPath();
    const { canvasX: originX, canvasY: originY } = worldToCanvas(0, 0);
    ctx.moveTo(0, originY);
    ctx.lineTo(canvas.width, originY);
    ctx.moveTo(originX, 0);
    ctx.lineTo(originX, canvas.height);
    ctx.stroke();

    if (mode === "waypoint" && displayWaypoints.length >= 2) {
      ctx.beginPath();
      ctx.strokeStyle = "#1890ff";
      ctx.lineWidth = 2;
      ctx.setLineDash([]);
      const firstWorld = latLonToWorld(displayWaypoints[0].lat, displayWaypoints[0].lon);
      const first = worldToCanvas(firstWorld.x, firstWorld.y);
      ctx.moveTo(first.canvasX, first.canvasY);
      for (let i = 1; i < displayWaypoints.length; i++) {
        const wpWorld = latLonToWorld(displayWaypoints[i].lat, displayWaypoints[i].lon);
        const pt = worldToCanvas(wpWorld.x, wpWorld.y);
        ctx.lineTo(pt.canvasX, pt.canvasY);
      }
      ctx.stroke();
    }

    if (mode === "waypoint") {
      displayWaypoints.forEach((wp, index) => {
        const wpWorld = latLonToWorld(wp.lat, wp.lon);
        const { canvasX, canvasY } = worldToCanvas(wpWorld.x, wpWorld.y);
        ctx.beginPath();
        ctx.arc(canvasX, canvasY, 6, 0, 2 * Math.PI);
        ctx.fillStyle = "#1890ff";
        ctx.fill();
        ctx.strokeStyle = "#fff";
        ctx.lineWidth = 2;
        ctx.stroke();
        ctx.fillStyle = "#fff";
        ctx.font = "bold 10px Arial";
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(String(index + 1), canvasX, canvasY);
      });
    }

    const { canvasX: curX, canvasY: curY } = worldToCanvas(
      currentPoint.x,
      currentPoint.y,
    );
    ctx.beginPath();
    ctx.arc(curX, curY, 8, 0, 2 * Math.PI);
    ctx.fillStyle = "#f5222d";
    ctx.fill();
    ctx.strokeStyle = "#fff";
    ctx.lineWidth = 2;
    ctx.stroke();

    const arrowLength = 20;
    const angle = currentPoint.yaw;
    const dirX = Math.sin(angle);
    const dirY = -Math.cos(angle);
    ctx.beginPath();
    ctx.moveTo(curX, curY);
    ctx.lineTo(curX + dirX * arrowLength, curY + dirY * arrowLength);
    ctx.strokeStyle = "#f5222d";
    ctx.lineWidth = 3;
    ctx.stroke();

    if (mode === "follow" && followState.startPoint) {
      const { canvasX: startX, canvasY: startY } = worldToCanvas(
        followState.startPoint.x,
        followState.startPoint.y,
      );
      ctx.beginPath();
      ctx.arc(startX, startY, 6, 0, 2 * Math.PI);
      ctx.fillStyle = "#52c41a";
      ctx.fill();
      ctx.strokeStyle = "#fff";
      ctx.lineWidth = 2;
      ctx.stroke();

      if (followState.currentMousePoint) {
        const { canvasX: currX, canvasY: currY } = worldToCanvas(
          followState.currentMousePoint.x,
          followState.currentMousePoint.y,
        );
        ctx.beginPath();
        ctx.moveTo(startX, startY);
        ctx.lineTo(currX, currY);
        ctx.strokeStyle = "#52c41a";
        ctx.lineWidth = 2;
        ctx.setLineDash([5, 3]);
        ctx.stroke();
        ctx.setLineDash([]);
      }
    }

    const interaction = interactionRef.current;
    if (interaction.type === "dragging-waypoint" && interaction.waypointId) {
      const wp = displayWaypoints.find((w) => w.id === interaction.waypointId);
      if (wp) {
        const wpWorld = latLonToWorld(wp.lat, wp.lon);
        const { canvasX, canvasY } = worldToCanvas(wpWorld.x, wpWorld.y);
        ctx.beginPath();
        ctx.arc(canvasX, canvasY, 10, 0, 2 * Math.PI);
        ctx.strokeStyle = "#faad14";
        ctx.lineWidth = 3;
        ctx.stroke();
      }
    }
  }, [
    originRef,
    isLoading,
    drawGrid,
    mode,
    displayWaypoints,
    followState.startPoint,
    followState.currentMousePoint,
    latLonToWorld,
  ]);

  useEffect(() => {
    drawCanvasRef.current = drawCanvas;
  });

  useEffect(() => {
    let rafId: number;
    const loop = () => {
      drawCanvasRef.current?.();
      rafId = requestAnimationFrame(loop);
    };
    rafId = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(rafId);
  }, []);

  // 状态变化时请求重绘
  useEffect(() => {
    requestRedraw();
  }, [mode, displayWaypoints, followState, isLoading, requestRedraw]);

  const handleMouseDown = useCallback(
    (e: React.MouseEvent<HTMLCanvasElement>) => {
      e.preventDefault();
      if (!canvasRef.current) return;

      const rect = canvasRef.current.getBoundingClientRect();
      const canvasX = e.clientX - rect.left;
      const canvasY = e.clientY - rect.top;
      const worldPos = canvasToWorld(canvasX, canvasY);

      if (mode === "waypoint") {
        if (e.button === 0) {
          const hit = showWaypointPanel ? waypoints.find((wp) => {
            const wpWorld = latLonToWorld(wp.lat, wp.lon);
            const { canvasX: wpX, canvasY: wpY } = worldToCanvas(
              wpWorld.x,
              wpWorld.y,
            );
            return Math.hypot(canvasX - wpX, canvasY - wpY) < HIT_TOLERANCE;
          }) : null;

          if (hit) {
            if (!isEditing) setIsEditing(true);
            const hitWorld = latLonToWorld(hit.lat, hit.lon);
            pushToHistory(waypoints);
            interactionRef.current = {
              type: "dragging-waypoint",
              waypointId: hit.id,
              startWorld: worldPos,
              startCanvas: { x: canvasX, y: canvasY },
              startOffset: {
                dx: hitWorld.x - worldPos.x,
                dy: hitWorld.y - worldPos.y,
              },
            };
          } else {
            interactionRef.current = {
              type: "none",
              startWorld: worldPos,
              startCanvas: { x: canvasX, y: canvasY },
            };
          }
        } else if (e.button === 2) {
          interactionRef.current = {
            type: "panning",
            startWorld: worldPos,
            startCanvas: { x: canvasX, y: canvasY },
          };
        }
      } else if (mode === "follow") {
        if (e.button === 0) {
          setFollowState((prev) => ({
            ...prev,
            isDrawing: true,
            startPoint: worldPos,
            currentMousePoint: worldPos,
          }));
        } else if (e.button === 2) {
          interactionRef.current = {
            type: "panning",
            startWorld: worldPos,
            startCanvas: { x: canvasX, y: canvasY },
          };
        }
      }
    },
    [isEditing, latLonToWorld, mode, pushToHistory, waypoints, showWaypointPanel],
  );

  const handleMouseMove = useCallback(
    (e: React.MouseEvent<HTMLCanvasElement>) => {
      if (!canvasRef.current) return;
      const rect = canvasRef.current.getBoundingClientRect();
      const canvasX = e.clientX - rect.left;
      const canvasY = e.clientY - rect.top;
      const worldPos = canvasToWorld(canvasX, canvasY);

      const interaction = interactionRef.current;

      if (interaction.type === "panning" && interaction.startCanvas) {
        const dx = canvasX - interaction.startCanvas.x;
        const dy = canvasY - interaction.startCanvas.y;
        viewRef.current = {
          ...viewRef.current,
          offsetX: viewRef.current.offsetX + dx,
          offsetY: viewRef.current.offsetY + dy,
        };
        interactionRef.current = {
          ...interaction,
          startCanvas: { x: canvasX, y: canvasY },
        };
        requestRedraw();
        return;
      }

      if (mode === "waypoint") {
        if (
          interaction.type === "dragging-waypoint" &&
          interaction.waypointId &&
          interaction.startOffset
        ) {
          if (!isEditing) {
            setIsEditing(true);
          }
          if (waypoints.findIndex((v) => v.id == interaction.waypointId) == 0)
            return;
          const newWorldX = worldPos.x + interaction.startOffset.dx;
          const newWorldY = worldPos.y + interaction.startOffset.dy;
          const newLatLon = worldToLatLon(newWorldX, newWorldY);
          setWaypoints((prev) =>
            prev.map((wp) =>
              wp.id === interaction.waypointId
                ? { ...wp, lat: newLatLon.lat, lon: newLatLon.lon }
                : wp,
            ),
          );
          return;
        }

        if (
          interaction.type === "none" &&
          interaction.startCanvas &&
          e.buttons === 1
        ) {
          const dist = Math.hypot(
            canvasX - interaction.startCanvas.x,
            canvasY - interaction.startCanvas.y,
          );
          if (dist > DRAG_THRESHOLD) {
            interactionRef.current = {
              type: "panning",
              startWorld: interaction.startWorld,
              startCanvas: interaction.startCanvas,
            };
          }
        }
      } else if (mode === "follow") {
        if (followState.isDrawing) {
          setFollowState((prev) => ({ ...prev, currentMousePoint: worldPos }));
        }
      }
      // drawCanvas is intentionally not in deps - it's called manually after ref updates
    },
    [
      mode,
      requestRedraw,
      isEditing,
      waypoints,
      worldToLatLon,
      followState.isDrawing,
    ],
  );
  const handleMouseUp = useCallback(
    (e: React.MouseEvent<HTMLCanvasElement>) => {
      if (!canvasRef.current) return;

      const rect = canvasRef.current.getBoundingClientRect();
      const canvasX = e.clientX - rect.left;
      const canvasY = e.clientY - rect.top;

      if (mode === "waypoint") {
        const interaction = interactionRef.current;
        if (interaction.type === "dragging-waypoint") {
          pushToHistory(waypointsRef.current);
        }

        if (
          e.button === 0 &&
          interaction.type === "none" &&
          interaction.startCanvas &&
          showWaypointPanel
        ) {
          if (!isEditing) setIsEditing(true);
          const dist = Math.hypot(
            canvasX - interaction.startCanvas.x,
            canvasY - interaction.startCanvas.y,
          );

          if (dist <= DRAG_THRESHOLD) {
            const worldPos = canvasToWorld(canvasX, canvasY);
            setWaypoints((prev) => {
              let curWP = [...prev];
              const createWp = (lat: number, lon: number): Waypoint => ({
                id: `wp-${Date.now()}-${Math.random()}`,
                lat,
                lon,
                height: DEFAULT_HEIGHT,
              });

              const origin = originRef.current;
              const originLat = origin?.lat || 0;
              const originLon = origin?.lon || 0;

              if (prev.length == 0) {
                curWP = [createWp(originLat, originLon)];
              }

              const newLatLon = worldToLatLon(worldPos.x, worldPos.y);
              const newWaypoints = [
                ...curWP,
                createWp(newLatLon.lat, newLatLon.lon),
              ];
              pushToHistory(newWaypoints);
              return newWaypoints;
            });
          }
        }
        interactionRef.current = { type: "none" };
      } else if (mode === "follow") {
        if (followState.isDrawing && e.button === 0) {
          setFollowState((prev) => ({
            ...prev,
            isDrawing: false,
            isFollowing: true,
          }));
        }
        if (e.button === 2) {
          interactionRef.current = { type: "none" };
        }
      }
    },
    [
      mode,
      pushToHistory,
      isEditing,
      originRef,
      worldToLatLon,
      followState.isDrawing,
      showWaypointPanel,
    ],
  );

  const handleMouseLeave = useCallback(() => {
    if (mode === "follow" && followState.isDrawing) {
      setFollowState((prev) => ({ ...prev, isDrawing: false }));
    }
    interactionRef.current = { type: "none" };
  }, [mode, followState.isDrawing]);

  useEffect(() => {
    const handleWheel = (e: WheelEvent) => {
      e.preventDefault();
      e.stopPropagation();
      const rect = canvasRef.current?.getBoundingClientRect();
      if (!rect) return;
      const mouseCanvasX = e.clientX - rect.left;
      const mouseCanvasY = e.clientY - rect.top;
      const worldPos = canvasToWorld(mouseCanvasX, mouseCanvasY);

      const delta = e.deltaY > 0 ? 0.9 : 1.1;
      const newScale = Math.max(
        1,
        Math.min(100, viewRef.current.scale * delta),
      );

      viewRef.current = {
        scale: newScale,
        offsetX:
          viewRef.current.offsetX -
          (worldPos.x * newScale - worldPos.x * viewRef.current.scale),
        offsetY:
          viewRef.current.offsetY +
          (worldPos.y * newScale - worldPos.y * viewRef.current.scale),
      };
      requestRedraw();
    };
    canvasRef.current?.addEventListener("wheel", handleWheel, {
      passive: false,
    });
  }, [requestRedraw]);

  const handleContextMenu = useCallback((e: React.MouseEvent) => {
    e.preventDefault();
  }, []);

  useEffect(() => {
    if (
      mode === "follow" &&
      followState.isFollowing &&
      followState.startPoint
    ) {
      if (followIntervalRef.current) clearInterval(followIntervalRef.current);
      followIntervalRef.current = setInterval(() => {
        const origin = originRef.current;
        if (!origin) return;
        const direction = followState.currentMousePoint
          ? {
              dx: followState.currentMousePoint.x - followState.startPoint!.x,
              dy: followState.currentMousePoint.y - followState.startPoint!.y,
            }
          : { dx: 0, dy: 0 };
        const deltaLon = direction.dx / METERS_PER_DEGREE_LON(origin.lat); // 经度差（度）
        const deltaLat = direction.dy / METERS_PER_DEGREE_LAT;             // 纬度差（度）
        const yaw = Math.atan2(deltaLon, deltaLat);                          // 弧度，范围 -π..π
        const data = {
          pos: [
            followState.startPoint!.x / METERS_PER_DEGREE_LON(origin.lat) +
              origin.lon,
            followState.startPoint!.y / METERS_PER_DEGREE_LAT + origin.lat,
            followState.followHeight,
          ],
          yaw,
          fix_yaw: followState.fixedHeading,
          vel: followState.followSpeed,
        };
        httpRequest("POST", followSubmitUrl, data);
      }, FOLLOW_INTERVAL_MS);
    } else {
      if (followIntervalRef.current) {
        clearInterval(followIntervalRef.current);
        followIntervalRef.current = null;
      }
    }
    return () => {
      if (followIntervalRef.current) clearInterval(followIntervalRef.current);
    };
  }, [
    mode,
    followState.isFollowing,
    followState.startPoint,
    followState.currentMousePoint,
    followState.fixedHeading,
    followState.followHeight,
    followSubmitUrl,
    originRef,
  ]);

  const submitWaypoints = useCallback(() => {
    if (waypoints.length === 0) {
      message.warning("没有航点可提交");
      return;
    }
    const data = waypoints.map((wp) => [wp.lon, wp.lat, wp.height]);
    httpRequest("POST", waypointSubmitUrl, { waypoint: data })
      .then(() => {
        message.success("航点提交成功");
        setIsEditing(false);
        setShowWaypointPanel(false);
      })
      .catch(() => message.error("航点提交失败"));
  }, [waypoints, waypointSubmitUrl]);

  const moveWaypoint = (index: number, direction: "up" | "down") => {
    setIsEditing(true);
    if (index == 0) return;
    if (direction === "up" && index === 0) return;
    if (direction === "down" && index === waypoints.length - 1) return;
    const newWaypoints = [...waypoints];
    const targetIndex = direction === "up" ? index - 1 : index + 1;
    [newWaypoints[index], newWaypoints[targetIndex]] = [
      newWaypoints[targetIndex],
      newWaypoints[index],
    ];
    pushToHistory(newWaypoints);
    setWaypoints(newWaypoints);
  };

  const columns: ColumnsType<Waypoint> = [
    {
      title: "序号",
      dataIndex: "id",
      key: "index",
      render: (_, __, index) => index + 1,
      width: 60,
    },
    {
      title: "经度",
      dataIndex: "lon",
      key: "lon",
      render: (val: number) => val.toFixed(6),
    },
    {
      title: "纬度",
      dataIndex: "lat",
      key: "lat",
      render: (val: number) => val.toFixed(6),
    },
    {
      title: "高度",
      dataIndex: "height",
      key: "height",
      render: (val: number, record) => (
        <InputNumber
          value={val}
          onChange={(value) => {
            setIsEditing(true);
            const newVal = Number(value);
            setWaypoints((prev) =>
              prev.map((wp) =>
                wp.id === record.id ? { ...wp, height: newVal } : wp,
              ),
            );
          }}
          min={0}
          step={1}
        />
      ),
    },
    {
      title: "顺序",
      key: "order",
      width: 100,
      render: (_, __, index) => (
        <Space size="small">
          <Button
            size="small"
            icon={<span>↑</span>}
            onClick={() => moveWaypoint(index, "up")}
            disabled={index === 0}
          />
          <Button
            size="small"
            icon={<span>↓</span>}
            onClick={() => moveWaypoint(index, "down")}
            disabled={index === waypoints.length - 1}
          />
        </Space>
      ),
    },
    {
      title: "操作",
      key: "action",
      render: (_, record) => (
        <Button
          type="link"
          danger
          onClick={() => {
            setIsEditing(true);
            pushToHistory(waypoints.filter((wp) => wp.id !== record.id));
            setWaypoints((prev) => prev.filter((wp) => wp.id !== record.id));
          }}
        >
          删除
        </Button>
      ),
    },
  ];

  const stopFollow = useCallback(() => {
    setFollowState((prev) => ({
      ...prev,
      isFollowing: false,
      startPoint: null,
      currentMousePoint: null,
    }));
  }, []);

  const clearWaypoints = useCallback(() => {
    setIsEditing(true);
    pushToHistory([]);
    setWaypoints([]);
  }, [pushToHistory]);

  const handleModeChange = useCallback(
    (newMode: "waypoint" | "follow") => {
      setMode(newMode);
      if (newMode === "follow") {
        setFollowState((prev) => ({
          ...prev,
          isDrawing: false,
          startPoint: null,
          currentMousePoint: null,
          fixedHeading: false,
          followHeight: DEFAULT_HEIGHT,
          isFollowing: false,
        }));
      } else {
        stopFollow();
      }
    },
    [stopFollow],
  );

  const btnRef = useRef<HTMLButtonElement>(null);
  useEffect(() => {
    btnRef.current?.addEventListener("click", () => {
      const { lat, lon } = useAppStore.getState().stateData;
      if (lat != null && lon != null) {
        originRef.current = {
          lat: lat as number,
          lon: lon as number,
        };
      }
      viewRef.current = {
        offsetX: 0,
        offsetY: 0,
        scale: DEFAULT_SCALE,
      };
      requestRedraw();
    });
  }, [originRef, requestRedraw]);

  return (
    <div className="waypoint-editor-fullscreen" ref={containerRef}>
      <div className="canvas-container-fullscreen">
        <canvas
          ref={canvasRef}
          className="canvas"
          style={{ cursor: showWaypointPanel ? "crosshair" : "grab" }}
          onMouseDown={handleMouseDown}
          onMouseMove={handleMouseMove}
          onMouseUp={handleMouseUp}
          onMouseLeave={handleMouseLeave}
          onContextMenu={handleContextMenu}
        />
        <div className="canvas-controls">
          <Button size="small" ref={btnRef}>
            重置视图
          </Button>
        </div>

        {/* 右侧悬浮的航点编辑按钮 */}
        <div style={{
          position: 'absolute',
          top: '50%',
          right: '16px',
          transform: 'translateY(-50%)',
          zIndex: 5
        }}>
          <Button 
            type="primary"
            size="large"
            onClick={() => setShowWaypointPanel(!showWaypointPanel)}
            style={{
              boxShadow: '0 4px 12px rgba(0,0,0,0.15)'
            }}
          >
            {showWaypointPanel ? '关闭编辑' : '航点编辑'}
          </Button>
        </div>
      </div>

      {/* 底部航点编辑面板 */}
      {showWaypointPanel && (
        <div style={{
          position: 'absolute',
          bottom: 0,
          left: 0,
          right: 0,
          height: '40vh',
          backgroundColor: 'rgba(255, 255, 255, 0.95)',
          backdropFilter: 'blur(8px)',
          boxShadow: '0 -4px 16px rgba(0,0,0,0.15)',
          zIndex: 25, // 提高层级，确保在 ButtonGroup (z-20) 之上
          padding: '16px',
          overflowY: 'auto'
        }}>
          <div className="mode-switch" style={{ marginBottom: '16px' }}>
            <Radio.Group
              value={mode}
              onChange={(e) => handleModeChange(e.target.value)}
              buttonStyle="solid"
            >
              <Radio.Button value="waypoint">航点模式</Radio.Button>
              <Radio.Button value="follow">跟随模式</Radio.Button>
            </Radio.Group>
          </div>

          {mode === "waypoint" && (
            <>
              <div className="table-header" style={{ marginBottom: '8px' }}>
                <span>航点列表</span>
                <div className="flex flex-row gap-2">
                  <Button
                    size="small"
                    onClick={undo}
                    disabled={historyRef.current.undoStack.length <= 1}
                  >
                    撤销
                  </Button>
                  <Button
                    size="small"
                    onClick={redo}
                    disabled={historyRef.current.redoStack.length === 0}
                  >
                    重做
                  </Button>
                  <Button size="small" onClick={clearWaypoints}>
                    清空
                  </Button>
                </div>
              </div>
              <Table
                dataSource={displayWaypoints}
                columns={columns}
                rowKey="id"
                size="small"
                pagination={false}
                scroll={{ y: 200 }}
                rowClassName={(_, index) => {
                  const wpIdx = wp_idx as number | undefined;
                  if (wpIdx !== undefined && wpIdx === index) {
                    return "waypoint-highlight";
                  }
                  return "";
                }}
              />

              <div className="mt-3 flex-row flex justify-center">
                <Space>
                  <Button
                    type="primary"
                    onClick={submitWaypoints}
                    disabled={waypoints.length < 2}
                  >
                    提交航点
                  </Button>
                  {isEditing && (
                    <Button onClick={handleCancelEdit}>取消编辑</Button>
                  )}
                </Space>
              </div>
            </>
          )}

          {mode === "follow" && (
            <div className="follow-panel">
              <div className="follow-row">
                <span>固定航向</span>
                <Switch
                  checked={followState.fixedHeading}
                  onChange={(checked) =>
                    setFollowState((prev) => ({
                      ...prev,
                      fixedHeading: checked,
                    }))
                  }
                />
              </div>
              <div className="follow-row">
                <span>跟随高度</span>
                <InputNumber
                  mode="spinner"
                  value={followState.followHeight}
                  onChange={(val) =>
                    setFollowState((prev) => ({
                      ...prev,
                      followHeight: val || 0,
                    }))
                  }
                  min={0}
                  step={1}
                />
              </div>
              <div className="follow-row">
                <span>跟随速度</span>
                <InputNumber
                  mode="spinner"
                  value={followState.followSpeed}
                  onChange={(val) =>
                    setFollowState((prev) => ({
                      ...prev,
                      followSpeed: val || 0,
                    }))
                  }
                  min={0}
                  step={1}
                />
              </div>
              <div className="follow-row">
                <Button
                  type="primary"
                  danger={followState.isFollowing}
                  onClick={stopFollow}
                  disabled={!followState.isFollowing}
                >
                  停止跟随
                </Button>
              </div>
              {followState.isFollowing && (
                <div className="follow-status">
                  跟随中... 方向:{" "}
                  {followState.currentMousePoint
                    ? `(${(followState.currentMousePoint.x - (followState.startPoint?.x || 0)).toFixed(2)}, ${(
                        followState.currentMousePoint.y -
                        (followState.startPoint?.y || 0)
                      ).toFixed(2)})`
                    : "无"}
                </div>
              )}
            </div>
          )}
        </div>
      )}
    </div>
  );
};


export default WaypointEditor;
