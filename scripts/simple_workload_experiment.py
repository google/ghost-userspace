#!/usr/bin/python3
import argparse


parser = argparse.ArgumentParser()
parser.add_argument(
    "--orca_port", type=int, required=True, help="the port of the running Orca server"
)


def run_experiment() -> None:
    "Run the experiment and return the CSV portion of the results."
    pass


def main() -> None:
    args = parser.parse_args()
    print(type(args.orca_port))


if __name__ == "__main__":
    main()
