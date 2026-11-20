import argparse
import asyncio
from pathlib import Path

from smpclient import SMPClient
from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite
from smpclient.requests.os_management import EchoWrite, ResetWrite
from smpclient.transport.udp import SMPUDPTransport


async def with_client(ip: str):
    return SMPClient(SMPUDPTransport(mtu=1500), ip, timeout_s=5)


async def list_images(ip: str) -> None:
    async with await with_client(ip) as client:
        response = await client.request(ImageStatesRead())
        for image in response.images:
            hash_hex = image.hash.hex() if image.hash else ""
            print(
                f"slot={image.slot} hash={hash_hex} "
                f"active={bool(image.active)} confirmed={bool(image.confirmed)} "
                f"pending={bool(image.pending)}"
            )


async def upload(ip: str, image_path: Path) -> None:
    image = image_path.read_bytes()
    async with await with_client(ip) as client:
        last_reported = -1
        async for offset in client.upload(image, slot=1, upgrade=False, first_timeout_s=40):
            if offset - last_reported >= 65536 or offset == len(image):
                print(f"Uploaded {offset}/{len(image)} bytes")
                last_reported = offset


async def set_test(ip: str, image_hash: str) -> None:
    async with await with_client(ip) as client:
        response = await client.request(
            ImageStatesWrite(hash=bytes.fromhex(image_hash), confirm=False)
        )
        print(response)


async def confirm(ip: str, image_hash: str) -> None:
    async with await with_client(ip) as client:
        response = await client.request(
            ImageStatesWrite(hash=bytes.fromhex(image_hash), confirm=True)
        )
        print(response)


async def reset(ip: str) -> None:
    async with await with_client(ip) as client:
        response = await client.request(ResetWrite())
        print(response)


async def upload_test_reset(ip: str, image_path: Path) -> None:
    await upload(ip, image_path)

    async with await with_client(ip) as client:
        state = await client.request(ImageStatesRead())
        candidate = next(
            (
                image
                for image in state.images
                if image.hash and not image.active
            ),
            None,
        )
        if candidate is None or candidate.hash is None:
            raise RuntimeError("No inactive uploaded image found")

        image_hash = candidate.hash
        test_response = await client.request(
            ImageStatesWrite(hash=image_hash, confirm=False)
        )
        print(test_response)
        print(f"Testing image: {image_hash.hex()}")

        reset_response = await client.request(ResetWrite())
        print(reset_response)


async def confirm_current(ip: str) -> None:
    async with await with_client(ip) as client:
        state = await client.request(ImageStatesRead())
        candidate = next(
            (
                image
                for image in state.images
                if image.active
            ),
            None,
        )
        if candidate is None:
            raise RuntimeError("No active image found")

        response = await client.request(
            ImageStatesWrite(hash=candidate.hash, confirm=True)
            if candidate.hash
            else ImageStatesWrite(confirm=True)
        )
        print(response)


async def probe(ip: str) -> None:
    async with await with_client(ip) as client:
        response = await client.request(EchoWrite(d="OTA probe"))
        print(response)


def main() -> None:
    parser = argparse.ArgumentParser(description="Solar Node UDP SMP OTA utility")
    parser.add_argument("--ip", required=True)
    subcommands = parser.add_subparsers(dest="command", required=True)

    subcommands.add_parser("list")
    subcommands.add_parser("probe")
    subcommands.add_parser("reset")
    subcommands.add_parser("confirm-current")

    upload_parser = subcommands.add_parser("upload")
    upload_parser.add_argument(
        "image",
        nargs="?",
        type=Path,
        default=Path("build/solar-node/zephyr/zephyr.signed.bin"),
    )

    test_parser = subcommands.add_parser("test")
    test_parser.add_argument("hash")

    confirm_parser = subcommands.add_parser("confirm")
    confirm_parser.add_argument("hash")

    test_upload_parser = subcommands.add_parser("upload-test-reset")
    test_upload_parser.add_argument(
        "image",
        nargs="?",
        type=Path,
        default=Path("build/solar-node/zephyr/zephyr.signed.bin"),
    )

    args = parser.parse_args()

    if args.command == "list":
        asyncio.run(list_images(args.ip))
    elif args.command == "upload":
        asyncio.run(upload(args.ip, args.image))
    elif args.command == "test":
        asyncio.run(set_test(args.ip, args.hash))
    elif args.command == "confirm":
        asyncio.run(confirm(args.ip, args.hash))
    elif args.command == "reset":
        asyncio.run(reset(args.ip))
    elif args.command == "probe":
        asyncio.run(probe(args.ip))
    elif args.command == "upload-test-reset":
        asyncio.run(upload_test_reset(args.ip, args.image))
    elif args.command == "confirm-current":
        asyncio.run(confirm_current(args.ip))


if __name__ == "__main__":
    main()
