module inspection_tuple

record Pair { left: Int64, right: Int64 }
record Box<T> { value: T }
enum Choice { yes(value: Pair), no }
enum Generic<T> { item(value: Box<T>) }
enum Empty {}

function absurd(value: Empty) -> Int64 effects { pure } { return match value {} }

function generic(value: Generic<Int64>) -> Int64 effects { pure } {
    return match value { item(Box { value = selected }) => selected }
}

function inspect(value: (Choice, Bool)) -> Int64 effects { pure } {
    let tuple = ("tuple", 42, true,)
    let projected = tuple.1
    return match value {
        (yes(Pair { left = item }), true) if item > 0 => item + projected,
        (yes(Pair { left = _ }), true) => projected,
        (_, false) => 0,
        (no(), true) => 0
    }
}
