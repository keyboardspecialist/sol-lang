module run.failure

@entry
public function launch() -> () effects { panic } {
    panic "boom"
}
