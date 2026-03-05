import asyncio
import json
import websockets

# pip install websockets

async def chat():
    uri = "ws://xxx.xxx.xxx.xxx:18789"
    async with websockets.connect(uri) as ws:
        # 发消息
        await ws.send(json.dumps({
            "type": "message",
            "content": "现在几点了？",
            "chat_id": "python_test"
        }))

        # 等待回复
        resp = await ws.recv()
        data = json.loads(resp)
        print("AI回复:", data["content"])

asyncio.run(chat())