import socket
import sys

def tcp_client(host, port, message):
    # 创建一个 TCP/IP 套接字
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client_socket:
        # 连接到服务器
        client_socket.connect((host, port))
        print(f"Connected to {host}:{port}")

        # 发送消息
        client_socket.sendall(message.encode('utf-8'))
        print(f"Sent: {message}")

        # 接收响应
        data = client_socket.recv(1024)
        print(f"Received: {data.decode('utf-8')}")

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: python client.py <host> <port> <message>")
        sys.exit(1)

    HOST = sys.argv[1]
    PORT = int(sys.argv[2])
    MESSAGE = sys.argv[3]

    tcp_client(HOST, PORT, MESSAGE)