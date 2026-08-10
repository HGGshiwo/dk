export interface Point {
  x: number;
  y: number;
}

export interface Waypoint {
  id: string;
  // GPS 坐标
  lon: number;
  lat: number;
  alt: number;
  // ENU 绝对坐标 (与飞机的 pos_enu 保持相同坐标系)
  x: number;
  y: number;
  z: number;
}

export interface WaypointData {
  lat: number;
  lon: number;
  alt: number;
}

export interface ViewState {
  offsetX: number;
  offsetY: number;
  scale: number;
}

export interface FollowState {
  isDrawing: boolean;
  startPoint: Point | null;
  currentMousePoint: Point | null;
  fixedHeading: boolean;
  followHeight: number;
  followSpeed: number;
  isFollowing: boolean;
}

export interface WaypointEditorProps {
  waypointSubmitUrl?: string;
  followSubmitUrl?: string;
  onEditModeChange?: (isEditing: boolean) => void;
}

export const HIT_TOLERANCE = 10;
export const DRAG_THRESHOLD = 5;
export const FOLLOW_INTERVAL_MS = 500;
export const GRID_STEP = 10;
export const METERS_PER_DEGREE_LAT = 111320;
export const DEFAULT_HEIGHT = 10;
export const DEFAULT_SCALE = 10;

export const METERS_PER_DEGREE_LON = (lat: number) =>
  111320 * Math.cos((lat * Math.PI) / 180);
