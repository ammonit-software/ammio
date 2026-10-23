"""Minimal Modbus TCP slave for the integration tests (pymodbus 3.8, in a background thread)."""

import asyncio
import threading

from pymodbus.datastore import (
    ModbusSequentialDataBlock,
    ModbusServerContext,
    ModbusSlaveContext,
)
from pymodbus.server import ModbusTcpServer

# pymodbus function codes used to address the data areas
FC_COILS, FC_DISCRETE, FC_HOLDING, FC_INPUT_REGS = 1, 2, 3, 4


class ModbusSim:
    def __init__(self, port: int, size: int = 200):
        self.port = port
        self.slave = ModbusSlaveContext(
            co=ModbusSequentialDataBlock(0, [0] * size),
            di=ModbusSequentialDataBlock(0, [0] * size),
            hr=ModbusSequentialDataBlock(0, [0] * size),
            ir=ModbusSequentialDataBlock(0, [0] * size),
        )
        self.context = ModbusServerContext(slaves=self.slave, single=True)
        self.loop = None
        self.server = None
        self.thread = None

    def start(self):
        ready = threading.Event()

        async def serve():
            # The server binds to the running loop, so it must be created inside it
            self.server = ModbusTcpServer(self.context, address=("127.0.0.1", self.port))
            await self.server.serve_forever(background=True)

        def run():
            self.loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self.loop)
            self.loop.run_until_complete(serve())
            ready.set()
            self.loop.run_forever()
            self.loop.close()

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()
        assert ready.wait(5), "Modbus simulator did not start"

    def stop(self):
        if not self.loop:
            return
        asyncio.run_coroutine_threadsafe(self.server.shutdown(), self.loop).result(5)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(5)
        self.loop = None

    # Addresses are protocol addresses, as a Modbus client sees them
    def get(self, fc: int, address: int, count: int = 1) -> list[int]:
        return [int(v) for v in self.slave.getValues(fc, address, count)]

    def set(self, fc: int, address: int, values: list[int]):
        self.slave.setValues(fc, address, values)
