# ammio

C-based interface layer for testing critical software systems. Bridges test scripts and the System Under Test via protocol communication.

## Requirements

- CMake 3.10+
- nng library (nanomsg-next-gen)

```bash
# Install nng on Ubuntu/Debian
sudo apt install libnng-dev
```

## Installation

```bash
mkdir build && cd build
cmake ..
make
```

## Usage

```bash
./build/ammio --config /path/to/config.json
```

## License

MIT
