import copy from "copy-to-clipboard";
import { httpRequest } from "../utils";
import { message } from "antd";

export interface CopyConfig {
  config_type: "copy";
  url: string;
  method: string;
  key: string;
}
// 复制到剪贴板工具
// eslint-disable-next-line @typescript-eslint/no-explicit-any
export const copyToClip = async (config: CopyConfig, params: any) => {
  const res = await httpRequest(config.method, config.url, params);
  const text = JSON.stringify(res);
  const success = copy(text);
  if (success) {
    message.success(`${text}, 已复制到剪贴板!`);
  } else {
    message.error(`复制失败，错误: ${text}`);
  }
  return success;
};
