#!/usr/bin/env python3
"""Run the stage-1 ESP32--Nano TCP client on the Jetson Nano."""

import argparse
import logging
import signal
import threading

from gateway.tcp_client import GatewayConfig, NanoTcpClient


def parse_args():
    parser = argparse.ArgumentParser(
        description="ESP32 ground-station TCP client (stage 1 only)"
    )
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--log-level", default="INFO")
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

    client = NanoTcpClient(GatewayConfig(host=args.host, port=args.port))
    client.run(stop_event)


if __name__ == "__main__":
    main()
