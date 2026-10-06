module conformance.p44c_qualified_entry_source_reject

enum Failure { failed }

@entry
public function launch() -> Result<Int64, Failure> effects { pure }
ensures { success => result > 0 }
{
    return ok(42)
}
