#!/usr/bin/env python3
"""Run the ESP32 gateway with an optional MSPM0 command/status bridge."""

import argparse
import logging
import os
import signal
import sys
import threading

from gateway.car_serial_worker import CarSerialWorker, load_car_serial_link
from gateway.command_dispatcher import CommandDispatcher
from gateway.state_store import StateStore
from gateway.tcp_client import GatewayConfig, NanoTcpClient


def parse_args():
    parser = argparse.ArgumentParser(
        description="ESP32 TCP client and MSPM0 command/status bridge"
    )
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--log-level", default="INFO")
    parser.add_argument(
        "--serial-port",
        help="enable MSPM0 commands and status using this serial device",
    )
    parser.add_argument(
        "--mspm0-link-dir",
        default=os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "mspm0_link")
        ),
        help="directory containing the existing serial_link.py",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    stop_event = threading.Event()

    def request_stop(signum, frame):
        del signum, frame
        stop_event.set()

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    state_store = StateStore()
    command_dispatcher = CommandDispatcher(state_store)
    client = NanoTcpClient(
        GatewayConfig(host=args.host, port=args.port),
        state_store=state_store,
        command_dispatcher=command_dispatcher,
    )
    serial_thread = None
    if args.serial_port:
        try:
            link_factory = load_car_serial_link(args.mspm0_link_dir)
        except (ImportError, RuntimeError) as error:
            logging.error("cannot load mspm0_link: %s", error)
            return 2
        worker = CarSerialWorker(
            port=args.serial_port,
            link_factory=link_factory,
            state_store=state_store,
            command_dispatcher=command_dispatcher,
        )
        serial_thread = threading.Thread(
            target=worker.run,
            args=(stop_event,),
            name="car-serial-worker",
        )
        serial_thread.start()

    try:
        client.run(stop_event)
    finally:
        stop_event.set()
        if serial_thread is not None:
            serial_thread.join(2.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
