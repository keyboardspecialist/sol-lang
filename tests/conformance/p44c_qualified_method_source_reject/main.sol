module conformance.p44c_qualified_method_source_reject

enum Failure { failed }

trait Checked {
    function checked(self: borrow Self) -> Result<Int64, Failure> effects { pure }
}

implementation Checked for Int64 {
    function checked(self: borrow Self) -> Result<Int64, Failure> effects { pure }
    ensures { success => result == self }
    {
        return ok(self)
    }
}
