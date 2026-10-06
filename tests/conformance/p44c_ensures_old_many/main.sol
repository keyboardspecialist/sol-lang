module conformance.p44c_ensures_old_many

function plan00(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }
function plan01(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }
function plan02(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }
function plan03(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }
function plan04(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }
function plan05(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }
function plan06(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }
function plan07(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }
function plan08(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }
function plan09(value: Int64) -> Int64 effects { pure } ensures { result == old(value) } { return value }

@entry
public function launch() -> Int64 effects { pure } {
    return plan00(1) + plan01(2) + plan02(3) + plan03(4) + plan04(5)
        + plan05(6) + plan06(7) + plan07(8) + plan08(3) + plan09(3)
}
