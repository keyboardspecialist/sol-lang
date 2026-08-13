module example.services

use example.models.CounterId
use example.models.NonnegativeValue

public capability Reader {
    function read(value: Int64) -> Int64 effects { service.read<Self> }
}

public function read_value(reader: capability Reader, value: Int64) -> Int64
effects { service.read<reader> } {
    return reader.read(value)
}

public function preserve_id(value: CounterId) -> CounterId effects { pure } {
    return value
}

public function preserve_nonnegative(value: NonnegativeValue) -> NonnegativeValue
effects { pure } {
    return value
}
