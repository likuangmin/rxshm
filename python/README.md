# rxshm (Python)

SDR samples in shared memory: `ShmWriter` publishes a stream of IF samples in
a ring, `ShmReader` reads it live from another process. Standard library
only; Linux. The layout and protocol are in the repository's
[SPEC.md](https://github.com/likuangmin/rxshm/blob/main/SPEC.md).

    pip install "rxshm @ git+https://github.com/likuangmin/rxshm@v1.0.0#subdirectory=python"
