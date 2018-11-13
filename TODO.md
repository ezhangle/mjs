* Lexer
    - Use `wstring_view` for token text (it's kept alive by the `source_file`)
    - Better handling of e.g. number literals..
    - Missing stuff?
* Parser
    - Create `parse_test` (and move `test_semicolon_insertion` from interpreter test)
    - Test `source_extend` logic (could probably be more precise for expressions/statements)
* Better GC
    - Ensure thread safety (perhaps only GC type information registration is troublesome)
    - Improve speed
    - Support compacting the current heap?
    - Reduce/remove reliance on gc_heap::local_heap()
    - Make it harder to use incorrectly - more type safety possible? (remove 'unsafe' calls)
    - Support pointers inside objects
    - Allocator support (But it seems like gc_heap_ptr is too fancy to be compatible - a static to_pointer() function can't really be 'nicely' [it could of course use local_heap, but that's not nice])
* Finish global object
    - Date
        - mutators
        - time zone adjustments
        - to/from string
    - Probably other missing stuff and non-compliant implementations...
* Optimize `NumberToString()`
* Create example(s)
    - Embedding mjs (I.e. adding user-defined classes)
* Make it easy to evaluate Javascript expressions (e.g. String('12') + 34)
* Optimize nested functions (they only need to be processed once (?))
* AST optimizations (constant propagation etc.)
* Optimize Array(), etc.
* Compile under Linux
* Use `char16_t` instead of `wchar_t` (and update `wstring`/`wistringstream` etc. etc.)
* Avoid duplicating `duration_cast`/`typeid()` logic with `expression_type`/`statement_type`. Consider using std::variant.
* General refactoring, implement ES3, ES5, ES...., JIT, etc. :)