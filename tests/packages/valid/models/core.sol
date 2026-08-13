module example.models

use example.rules.nonnegative

public record Counter {
    value: Int64
}

public type CounterId = distinct Int64

public type NonnegativeValue = refined Int64 where nonnegative(self)

function normalize(value: Int64) -> Int64 effects { pure } {
    return value
}
