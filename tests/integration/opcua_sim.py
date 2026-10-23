"""Minimal OPC UA server for the integration tests (asyncua, in a background thread)."""

import asyncio
import logging
import threading

from asyncua import Server, ua

logging.getLogger("asyncua").setLevel(logging.ERROR)


class OpcuaSim:
    """Exposes ns=2;s=<name> variables with the given (value, VariantType)."""

    def __init__(self, port: int, variables: dict[str, tuple]):
        self.port = port
        self.variables = variables
        self.nodes = {}
        self.loop = None
        self.server = None
        self.thread = None

    @property
    def endpoint(self) -> str:
        return f"opc.tcp://127.0.0.1:{self.port}"

    async def _start(self):
        self.server = Server()
        await self.server.init()
        self.server.set_endpoint(f"opc.tcp://127.0.0.1:{self.port}/")
        self.server.set_security_policy([ua.SecurityPolicyType.NoSecurity])
        idx = await self.server.register_namespace("urn:ammio:test")
        assert idx == 2
        obj = await self.server.nodes.objects.add_object(idx, "Sim")
        for name, (value, vtype) in self.variables.items():
            node = await obj.add_variable(ua.NodeId(name, idx), name, ua.Variant(value, vtype))
            await node.set_writable()
            self.nodes[name] = node
        await self.server.start()

    def start(self):
        ready = threading.Event()
        error = []

        def run():
            self.loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self.loop)
            try:
                self.loop.run_until_complete(self._start())
            except Exception as e:  # surfaced to the test below
                error.append(e)
            ready.set()
            if not error:
                self.loop.run_forever()

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()
        assert ready.wait(15), "OPC UA simulator did not start"
        if error:
            raise error[0]

    async def _stop(self):
        # Behave like a crashed server: stop accepting first (ammio reconnects within one period,
        # and asyncua's stop() waits for every connected client), then drop the sessions.
        self.server.bserver._server.close()
        for client in list(self.server.bserver.clients):
            client.transport.abort()
        await self.server.stop()

    def stop(self):
        if not self.loop:
            return
        asyncio.run_coroutine_threadsafe(self._stop(), self.loop).result(30)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(10)
        self.loop = None

    def _run(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(5)

    def get(self, name: str):
        return self._run(self.nodes[name].read_value())

    def set(self, name: str, value):
        _, vtype = self.variables[name]
        self._run(self.nodes[name].write_value(ua.DataValue(ua.Variant(value, vtype))))
