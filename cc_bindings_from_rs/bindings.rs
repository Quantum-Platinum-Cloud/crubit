// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

use anyhow::{bail, Result};
use code_gen_utils::format_cc_ident;
use proc_macro2::TokenStream;
use quote::quote;
use rustc_interface::Queries;
use rustc_middle::dep_graph::DepContext;
use rustc_middle::middle::exported_symbols::ExportedSymbol;
use rustc_middle::ty::TyCtxt;
use rustc_span::def_id::{LocalDefId, LOCAL_CRATE};
use std::fmt::Display;

pub struct GeneratedBindings {
    pub h_body: TokenStream,
}

impl GeneratedBindings {
    pub fn generate(tcx: TyCtxt) -> Self {
        let top_comment = {
            let crate_name = tcx.crate_name(LOCAL_CRATE);
            let txt = format!(
                "Automatically @generated C++ bindings for the following Rust crate:\n\
                 {crate_name}"
            );
            quote! { __COMMENT__ #txt __NEWLINE__ }
        };

        let h_body = {
            let crate_content = format_crate(tcx).unwrap_or_else(|err| {
                let txt = format!("Failed to generate bindings for the crate: {}", err);
                quote! { __COMMENT__ #txt }
            });
            quote! {
                #top_comment
                #crate_content
            }
        };

        Self { h_body }
    }
}

/// Helper (used by `bindings_driver` and `test::run_compiler`) for invoking
/// functions operating on `TyCtxt`.
pub fn enter_tcx<'tcx, F, T>(
    queries: &'tcx Queries<'tcx>,
    f: F,
) -> rustc_interface::interface::Result<T>
where
    F: FnOnce(TyCtxt<'tcx>) -> T + Send,
    T: Send,
{
    let query_context = queries.global_ctxt()?;
    Ok(query_context.peek_mut().enter(f))
}

fn format_def(_tcx: TyCtxt, _def_id: LocalDefId) -> Result<TokenStream> {
    bail!("Nothing works yet!")
}

fn format_unsupported_def(
    tcx: TyCtxt,
    local_def_id: LocalDefId,
    err_msg: impl Display,
) -> TokenStream {
    let span = tcx.sess().source_map().span_to_embeddable_string(tcx.def_span(local_def_id));
    let name = tcx.def_path_str(local_def_id.to_def_id());
    let msg = format!("Error while generating bindings for `{name}` defined at {span}: {err_msg}");
    quote! { __NEWLINE__ __NEWLINE__ __COMMENT__ #msg __NEWLINE__ }
}

fn format_crate(tcx: TyCtxt) -> Result<TokenStream> {
    let crate_name = format_cc_ident(tcx.crate_name(LOCAL_CRATE).as_str())?;

    // TODO(lukasza): We probably shouldn't be using `exported_symbols` as the main
    // entry point for finding Rust definitions that need to be wrapping in C++
    // bindings.  For example, it _seems_ that things like `type` aliases or
    // `struct`s (without an `impl`) won't be visible to a linker and therefore
    // won't have exported symbols.
    let snippets =
        tcx.exported_symbols(LOCAL_CRATE).iter().filter_map(move |(symbol, _)| match symbol {
            ExportedSymbol::NonGeneric(def_id) => {
                // It seems that non-generic exported symbols should all be defined in the
                // `LOCAL_CRATE`.  Furthermore, `def_id` seems to be a `LocalDefId`.  OTOH, it
                // isn't clear why `ExportedSymbol::NonGeneric` holds a `DefId` rather than a
                // `LocalDefId`.  For now, we assert `expect_local` below (and if it fails, then
                // hopefully it will help us understand these things better and maybe add
                // extra unit tests against out code).
                let local_id = def_id.expect_local();

                Some(match format_def(tcx, local_id) {
                    Ok(snippet) => snippet,
                    Err(err) => format_unsupported_def(tcx, local_id, err),
                })
            }
            ExportedSymbol::Generic(def_id, _substs) => {
                // Ignore non-local defs.  Map local defs to an unsupported comment.
                //
                // We are guessing that a non-local `def_id` can happen when the `LOCAL_CRATE`
                // exports a monomorphization/specialization of a generic defined in a different
                // crate.  One specific example (covered via `async fn` in one of the tests) is
                // `DefId(2:14250 ~ core[ef75]::future::from_generator)`.
                def_id.as_local().map(|local_id| {
                    format_unsupported_def(tcx, local_id, "Generics are not supported yet.")
                })
            }
            ExportedSymbol::DropGlue(_) | ExportedSymbol::NoDefId(_) => None,
        });

    Ok(quote! {
        namespace #crate_name {
            #( #snippets )*
        }
    })
}

#[cfg(test)]
pub mod tests {
    use super::GeneratedBindings;

    use quote::quote;
    use std::path::PathBuf;

    use token_stream_matchers::{assert_cc_matches, assert_cc_not_matches};

    pub fn get_sysroot_for_testing() -> PathBuf {
        let runfiles = runfiles::Runfiles::create().unwrap();
        runfiles.rlocation(if std::env::var("LEGACY_TOOLCHAIN_RUST_TEST").is_ok() {
            "google3/third_party/unsupported_toolchains/rust/toolchains/nightly"
        } else {
            "google3/nowhere/llvm/rust"
        })
    }

    #[test]
    #[should_panic]
    fn test_infra_panic_when_test_input_contains_syntax_errors() {
        run_compiler("syntax error here", |_tcx| ())
    }

    #[test]
    #[should_panic]
    fn test_infra_panic_when_test_input_triggers_analysis_errors() {
        run_compiler("#![feature(no_such_feature)]", |_tcx| ())
    }

    #[test]
    fn test_infra_nightly_features_ok_in_test_input() {
        run_compiler("#![feature(yeet_expr)]", |_tcx| ())
    }

    #[test]
    fn test_generated_bindings_fn_success() {
        // This test covers only a single example of a function that should get a C++
        // binding. Additional coverage of functions items will be provided by
        // future, `format_def`-focused tests.
        let test_src = r#"
                pub extern "C" fn public_function() {
                    println!("foo");
                }
            "#;
        test_generated_bindings(test_src, |bindings| {
            // TODO(lukasza): Fix test expectations once this becomes supported (in early Q4
            // 2022).
            let expected_comment_txt = "Error while generating bindings for `public_function` \
                                        defined at <crubit_unittests.rs>:2:17: 2:52: \
                                        Nothing works yet!";
            assert_cc_matches!(
                bindings.h_body,
                quote! {
                    __COMMENT__ #expected_comment_txt
                }
            );
        });
    }

    #[test]
    fn test_generated_bindings_fn_non_pub() {
        let test_src = r#"
                extern "C" fn private_function() {
                    println!("foo");
                }
            "#;
        test_generated_bindings(test_src, |bindings| {
            // Non-public functions should not be present in the generated bindings.
            assert_cc_not_matches!(bindings.h_body, quote! { private_function });
        });
    }

    #[test]
    fn test_generated_bindings_top_level_items() {
        let test_src = "pub fn public_function() {}";
        test_generated_bindings(test_src, |bindings| {
            let expected_comment_txt =
                "Automatically @generated C++ bindings for the following Rust crate:\n\
                 rust_out";
            assert_cc_matches!(
                bindings.h_body,
                quote! {
                    __COMMENT__ #expected_comment_txt
                    ...
                    namespace rust_out {
                        ...
                    }
                }
            );
        })
    }

    #[test]
    fn test_generated_bindings_unsupported_item() {
        // This test verifies how `Err` from `format_def` is formatted as a C++ comment.
        // - This test covers only a single example of an unsupported item.  Additional
        //   coverage of unsupported items will be provided by future,
        //   `format_def`-focused tests.
        // - This test somewhat arbitrarily chooses an example of an unsupported item
        //   (i.e. if `async fn` becomes supported by `cc_bindings_from_rs` in the
        //   future, then the test will have to be modified to use another `test_src`
        //   input).
        let test_src = r#"
                pub async fn public_function() {}
            "#;

        test_generated_bindings(test_src, |bindings| {
            let expected_comment_txt = "Error while generating bindings for `public_function` \
                                        defined at <crubit_unittests.rs>:2:17: 2:47: \
                                        Nothing works yet!";
            assert_cc_matches!(
                bindings.h_body,
                quote! {
                    __COMMENT__ #expected_comment_txt
                }
            );
        })
    }

    fn test_generated_bindings<F, T>(source: &str, f: F) -> T
    where
        F: FnOnce(GeneratedBindings) -> T + Send,
        T: Send,
    {
        run_compiler(source, |tcx| f(GeneratedBindings::generate(tcx)))
    }

    /// Compiles Rust `source` then calls `f` on the `TyCtxt` representation
    /// of the compiled `source`.
    fn run_compiler<F, T>(source: impl Into<String>, f: F) -> T
    where
        F: for<'tcx> FnOnce(rustc_middle::ty::TyCtxt<'tcx>) -> T + Send,
        T: Send,
    {
        use rustc_session::config::{CrateType, Input, Options, OutputType, OutputTypes};

        const TEST_FILENAME: &str = "crubit_unittests.rs";

        // Setting `output_types` that will trigger code gen - otherwise some parts of
        // the analysis will be missing (e.g. `tcx.exported_symbols()`).
        // The choice of `Bitcode` is somewhat arbitrary (e.g. `Assembly`,
        // `Mir`, etc. would also trigger code gen).
        let output_types = OutputTypes::new(&[(OutputType::Bitcode, None /* PathBuf */)]);

        let opts = Options {
            crate_types: vec![CrateType::Rlib], // Test inputs simulate library crates.
            maybe_sysroot: Some(get_sysroot_for_testing()),
            output_types,
            edition: rustc_span::edition::Edition::Edition2021,
            unstable_features: rustc_feature::UnstableFeatures::Allow,
            ..Default::default()
        };

        let config = rustc_interface::interface::Config {
            opts,
            crate_cfg: Default::default(),
            crate_check_cfg: Default::default(),
            input: Input::Str {
                name: rustc_span::FileName::Custom(TEST_FILENAME.to_string()),
                input: source.into(),
            },
            input_path: None,
            output_file: None,
            output_dir: None,
            file_loader: None,
            diagnostic_output: rustc_session::DiagnosticOutput::Default,
            lint_caps: Default::default(),
            parse_sess_created: None,
            register_lints: None,
            override_queries: None,
            make_codegen_backend: None,
            registry: rustc_errors::registry::Registry::new(rustc_error_codes::DIAGNOSTICS),
        };

        use rustc_interface::interface::Result;
        let result: Result<Result<T>> =
            rustc_interface::interface::run_compiler(config, |compiler| {
                compiler.enter(|queries| {
                    super::enter_tcx(queries, |tcx| {
                        // Explicitly check the result of `analysis` stage to detect compilation
                        // errors that the earlier stages might miss.  This helps ensure that the
                        // test inputs are valid Rust (even if `f` wouldn't
                        // have triggered full analysis).
                        tcx.analysis(())?;

                        Ok(f(tcx))
                    })
                })
            });
        // Flatten the outer and inner results into a single result.  (outer result
        // comes from `enter_tcx`; inner result comes from `analysis`).
        let result: Result<T> = result.and_then(|result| result);

        result.expect("Test inputs shouldn't cause compilation errors")
    }
}
