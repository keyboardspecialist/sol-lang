module example.services

public capability Reader {
    function read(value: Int64) -> Int64 effects { service.read<Self> }
}

public function read_value(reader: capability Reader, value: Int64) -> Int64
effects { service.read<reader> } {
    return reader.read(value)
}
