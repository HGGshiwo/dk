import { useEffect, useRef, useCallback } from "react";
import { useAppStore } from "../../../store/useAppStore";
import {
  type Point,
  type Waypoint,
  type ViewState,
  type FollowState,
  GRID_STEP,
} from "../types";

interface UseCanvasRenderProps {
  canvasRef: React.RefObject<HTMLCanvasElement | null>;
  viewRef: React.RefObject<ViewState>;
  displayOriginRef: React.RefObject<Point>;
  mode: "waypoint" | "follow";
  displayWaypoints: Waypoint[];
  followState: FollowState;
  interactionRef: React.RefObject<{
    type: "none" | "dragging-waypoint" | "panning";
    waypointId?: string;
  }>;
  isLoading: boolean;
}

export const useCanvasRender = ({
  canvasRef,
  viewRef,
  displayOriginRef,
  mode,
  displayWaypoints,
  followState,
  interactionRef,
  isLoading,
}: UseCanvasRenderProps) => {
  const drawCanvasRef = useRef<() => void>(() => {});

  const worldToCanvas = useCallback(
    (worldX: number, worldY: number) => {
      const canvas = canvasRef.current;
      if (!canvas) return { canvasX: 0, canvasY: 0 };
      const rect = canvas.getBoundingClientRect();
      const centerX = rect.width / 2;
      const centerY = rect.height / 2;
      const { scale, offsetX, offsetY } = viewRef.current;
      const origin = displayOriginRef.current;

      return {
        canvasX: centerX + (worldX - origin.x) * scale + offsetX,
        canvasY: centerY - (worldY - origin.y) * scale + offsetY,
      };
    },
    [canvasRef, viewRef, displayOriginRef]
  );

  const canvasToWorld = useCallback(
    (canvasX: number, canvasY: number): Point => {
      const canvas = canvasRef.current;
      if (!canvas) return { x: 0, y: 0 };
      const rect = canvas.getBoundingClientRect();
      const centerX = rect.width / 2;
      const centerY = rect.height / 2;
      const { scale, offsetX, offsetY } = viewRef.current;
      const origin = displayOriginRef.current;

      return {
        x: (canvasX - centerX - offsetX) / scale + origin.x,
        y: -(canvasY - centerY - offsetY) / scale + origin.y,
      };
    },
    [canvasRef, viewRef, displayOriginRef]
  );

  const drawGrid = useCallback(
    (ctx: CanvasRenderingContext2D, canvas: HTMLCanvasElement) => {
      const topLeft = canvasToWorld(0, 0);
      const topRight = canvasToWorld(canvas.width, 0);
      const bottomLeft = canvasToWorld(0, canvas.height);
      const bottomRight = canvasToWorld(canvas.width, canvas.height);

      const minWorldX = Math.min(topLeft.x, topRight.x, bottomLeft.x, bottomRight.x);
      const maxWorldX = Math.max(topLeft.x, topRight.x, bottomLeft.x, bottomRight.x);
      const minWorldY = Math.min(topLeft.y, topRight.y, bottomLeft.y, bottomRight.y);
      const maxWorldY = Math.max(topLeft.y, topRight.y, bottomLeft.y, bottomRight.y);

      ctx.strokeStyle = "#ccc";
      ctx.lineWidth = 0.5;
      ctx.beginPath();

      const startX = Math.floor(minWorldX / GRID_STEP) * GRID_STEP;
      for (let x = startX; x <= maxWorldX; x += GRID_STEP) {
        const { canvasX } = worldToCanvas(x, 0);
        if (canvasX >= 0 && canvasX <= canvas.width) {
          ctx.moveTo(canvasX, 0);
          ctx.lineTo(canvasX, canvas.height);
        }
      }

      const startY = Math.floor(minWorldY / GRID_STEP) * GRID_STEP;
      for (let y = startY; y <= maxWorldY; y += GRID_STEP) {
        const { canvasY } = worldToCanvas(0, y);
        if (canvasY >= 0 && canvasY <= canvas.height) {
          ctx.moveTo(0, canvasY);
          ctx.lineTo(canvas.width, canvasY);
        }
      }
      ctx.stroke();
    },
    [canvasToWorld, worldToCanvas]
  );

  const drawCanvas = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;

    const rect = canvas.getBoundingClientRect();
    if (canvas.width !== rect.width || canvas.height !== rect.height) {
      canvas.width = rect.width;
      canvas.height = rect.height;
    }
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

    // 1. 绘制网格
    drawGrid(ctx, canvas);

    // 2. 绘制十字坐标轴 (基于 displayOrigin 原点)
    ctx.strokeStyle = "#333";
    ctx.lineWidth = 2;
    ctx.beginPath();
    const { canvasX: originX, canvasY: originY } = worldToCanvas(
      displayOriginRef.current.x,
      displayOriginRef.current.y
    );
    ctx.moveTo(0, originY);
    ctx.lineTo(canvas.width, originY);
    ctx.moveTo(originX, 0);
    ctx.lineTo(originX, canvas.height);
    ctx.stroke();

    // 3. 绘制航线
    if (mode === "waypoint" && displayWaypoints.length >= 2) {
      ctx.beginPath();
      ctx.strokeStyle = "#1890ff";
      ctx.lineWidth = 2;
      ctx.setLineDash([]);
      const first = worldToCanvas(displayWaypoints[0].x, displayWaypoints[0].y);
      ctx.moveTo(first.canvasX, first.canvasY);

      for (let i = 1; i < displayWaypoints.length; i++) {
        const pt = worldToCanvas(displayWaypoints[i].x, displayWaypoints[i].y);
        ctx.lineTo(pt.canvasX, pt.canvasY);
      }
      ctx.stroke();
    }

    // 4. 绘制航点
    if (mode === "waypoint") {
      displayWaypoints.forEach((wp, index) => {
        const { canvasX, canvasY } = worldToCanvas(wp.x, wp.y);
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

    // 5. 绘制当前无人机/车辆位置
    const stateData = useAppStore.getState().stateData;
    const statePosEnu = stateData.pos_enu as number[] | undefined;
    const yaw = Number(stateData.yaw) || 0;

    const currentPoint: Point & { yaw: number } = {
      x: statePosEnu && statePosEnu.length >= 2 ? statePosEnu[0] : displayOriginRef.current.x,
      y: statePosEnu && statePosEnu.length >= 2 ? statePosEnu[1] : displayOriginRef.current.y,
      yaw,
    };

    const { canvasX: curX, canvasY: curY } = worldToCanvas(
      currentPoint.x,
      currentPoint.y
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

    // 6. 绘制跟随模式连线
    if (mode === "follow" && followState.startPoint) {
      const { canvasX: startX, canvasY: startY } = worldToCanvas(
        followState.startPoint.x,
        followState.startPoint.y
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
          followState.currentMousePoint.y
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

    // 7. 拖拽航点时的高亮圈
    const interaction = interactionRef.current;
    if (interaction.type === "dragging-waypoint" && interaction.waypointId) {
      const wp = displayWaypoints.find((w) => w.id === interaction.waypointId);
      if (wp) {
        const { canvasX, canvasY } = worldToCanvas(wp.x, wp.y);
        ctx.beginPath();
        ctx.arc(canvasX, canvasY, 10, 0, 2 * Math.PI);
        ctx.strokeStyle = "#faad14";
        ctx.lineWidth = 3;
        ctx.stroke();
      }
    }
  }, [
    canvasRef,
    isLoading,
    drawGrid,
    worldToCanvas,
    displayOriginRef,
    mode,
    displayWaypoints,
    followState.startPoint,
    followState.currentMousePoint,
    interactionRef,
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

  const requestRedraw = useCallback(() => {
    drawCanvas();
  }, [drawCanvas]);

  return {
    worldToCanvas,
    canvasToWorld,
    requestRedraw,
  };
};
