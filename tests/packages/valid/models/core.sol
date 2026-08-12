module example.models

public record Counter {
    value: Int64
}

function normalize(value: Int64) -> Int64 effects { pure } {
    return value
}
