module inspection_generic

record Box<T> { value: T, }

function identity<T>(value: T) -> T effects { pure } {
    return value
}

function inspect(value: borrow Text) -> Int64 effects { pure } {
    return 1
}

function boxed() -> Box<Text> effects { pure } {
    return Box<Text> { value = identity<Text>("inspection") }
}
