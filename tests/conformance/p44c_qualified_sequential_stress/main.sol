module conformance.p44c_qualified_sequential_stress

enum Failure { failed }

function checked00(value: Int64) -> Result<Int64, Failure>
effects { pure }
ensures {
    success => result == value
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
}
{
    return ok(value)
}

function checked01(value: Int64) -> Result<Int64, Failure>
effects { pure }
ensures {
    success => result == value
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
}
{
    return ok(value)
}

function checked02(value: Int64) -> Result<Int64, Failure>
effects { pure }
ensures {
    success => result == value
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
}
{
    return ok(value)
}

function checked03(value: Int64) -> Result<Int64, Failure>
effects { pure }
ensures {
    success => result == value
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
    success => result > 0
    failure => 1 / 0 > 0
}
{
    return ok(value)
}

function second() -> Int64 effects { pure } {
    let ignored02 = checked02(40)
    let ignored03 = checked03(41)
    return 42
}

@entry
public function first() -> Int64 effects { pure } {
    let ignored00 = checked00(38)
    let ignored01 = checked01(39)
    let ignored_second = second()
    return 42
}
