module conformance.p44f_site_domains_snapshots
function checked(first: Int64, second: Int64) -> Int64 effects { pure }
ensures { result == old(first) - old(second) } {
    if first > 0 { return first - second } else { return first / second }
}
@entry
public function other() -> Int64 effects { pure } { return checked(-4, 1) }
