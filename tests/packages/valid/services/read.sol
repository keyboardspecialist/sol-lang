module example.services

public function read_value(value: Int64) -> Int64 effects { service.read } {
    return value
}
