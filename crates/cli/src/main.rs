// SPDX-License-Identifier: AGPL-3.0-or-later

//! PolyMesh CLI. Subcommands grow with the phases; `check` is the P0 stub.

use clap::{Parser, Subcommand};
use std::path::PathBuf;

#[derive(Parser)]
#[command(
    name = "polymesh",
    version,
    about = "Adaptive hybrid polyhedral mesher + FEA solver"
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Load a geometry file and report validity + basic statistics.
    Check {
        /// Input geometry (.stl).
        input: PathBuf,
    },
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    match Cli::parse().command {
        Command::Check { input } => {
            let surface = geom::stl::load_stl(&input)?;
            surface.validate()?;
            println!(
                "{}: OK — {} vertices, {} triangles",
                input.display(),
                surface.vertices.len(),
                surface.triangles.len()
            );
        }
    }
    Ok(())
}
