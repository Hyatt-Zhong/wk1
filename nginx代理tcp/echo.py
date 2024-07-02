import socket

def start_echo_server(host, port):
    # 创建一个 TCP/IP 套接字
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_socket:
        # 绑定套接字到地址和端口
        server_socket.bind((host, port))
        # 使服务器能够接受连接（监听队列大小为1）
        server_socket.listen(1)
        print(f"TCP Echo server is listening on {host}:{port}")

        while True:
            # 等待客户端连接
            client_socket, client_address = server_socket.accept()
            with client_socket:
                print(f"Connected by {client_address}")
                while True:
                    # 接收数据
                    data = client_socket.recv(1024)
                    if not data:
                        break
                    print(f"Received: {data.decode('utf-8')}")
                    # 回显收到的数据
                    client_socket.sendall(data)

if __name__ == "__main__":
    HOST = "127.0.0.1"  # 监听本地环回地址
    PORT = 54321        # 监听的端口号

    start_echo_server(HOST, PORT)