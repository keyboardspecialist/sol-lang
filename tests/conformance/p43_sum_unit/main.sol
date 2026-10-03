module conformance.p43_sum_unit

enum Marks { stamped(unit: ()), count(value: Int64), empty }

@entry
public function first() -> Int64 effects { pure } {
    let stamped = Marks.stamped(())
    let stamped_copy = stamped
    let seven = Marks.count(7)
    let seven_copy = seven
    let eight = Marks.count(8)
    let empty = Marks.empty
    let empty_copy = empty
    return if stamped == stamped_copy && seven == seven_copy && seven != eight
        && stamped != seven && seven != empty && empty == empty_copy { 42 } else { 0 }
}

function second() -> Int64 effects { pure } { return 0 }
