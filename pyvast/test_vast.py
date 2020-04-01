import aiounittest
import unittest

import asyncio
from vast import VAST
import json


class TestConnection(aiounittest.AsyncTestCase):
    def setUp(self):
        self.vast = VAST(binary="/opt/tenzir/bin/vast")

    async def test_connection(self):
        self.assertTrue(await self.vast.test_connection())


class TestCallStackCreation(unittest.TestCase):
    def setUp(self):
        self.vast = VAST(binary="/opt/tenzir/bin/vast")

    def test_call_chaining(self):
        self.assertEqual(self.vast.call_stack, [])
        query = ":addr == 192.168.1.104 && #timestamp < 1 hour ago"
        self.vast.export(continuous=True).json(query)
        self.assertEqual(
            self.vast.call_stack, ["export", "--continuous=True", "json", query]
        )

    def test_import_keyword(self):
        self.assertEqual(self.vast.call_stack, [])
        path = "/some/file"
        self.vast._import().pcap(read=path)
        self.assertEqual(self.vast.call_stack, ["import", "pcap", f"--read={path}"])

    def test_underscore_replacement(self):
        self.assertEqual(self.vast.call_stack, [])
        self.vast.export(max_events=10).json("192.168.1.104")
        self.assertEqual(
            self.vast.call_stack, ["export", "--max-events=10", "json", "192.168.1.104"]
        )
