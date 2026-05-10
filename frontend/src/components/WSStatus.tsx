import { Tag } from 'antd';
import { useAppStore } from '../store/useAppStore';

export const WSStatus = () => {
  const wsStatus = useAppStore(state => state.wsStatus);

  const getStatusTag = () => {
    switch (wsStatus) {
      case 'connecting': return { color: 'gold', text: 'WS连接中...' };
      case 'open': return { color: 'green', text: 'WS已连接' };
      case 'closed': return { color: 'red', text: 'WS已断开（自动重连中）' };
      case 'error': return { color: 'orange', text: 'WS连接错误' };
      default: return { color: 'gray', text: 'WS状态未知' };
    }
  };

  const { color, text } = getStatusTag();

  return (
    <div className="mb-2 flex items-center w-full">
      <Tag color={color} style={{fontSize: "16px", padding: "6px"}}>{text}</Tag>
    </div>
  );
};