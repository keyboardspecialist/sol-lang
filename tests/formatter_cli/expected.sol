module formatter.cli
record Box<T> { value: T, }
function identity<T>(value: T) -> T effects { pure } {
    return value
}
