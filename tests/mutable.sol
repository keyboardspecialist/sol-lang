module mutable

function update(flag: Bool) -> Int64 effects { pure } {
    var value = 1
    value = if flag { 2 } else { 3 }
    return value
}
