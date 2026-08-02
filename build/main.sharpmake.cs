using Sharpmake;

public class BaseTarget : Target
{
    public BaseTarget()
        : base(Platform.win64, DevEnv.vs2022, Optimization.Debug | Optimization.Release)
    {
    }
}

public abstract class OmdLabProjectBase : Project
{
    protected OmdLabProjectBase(string name)
        : base(typeof(BaseTarget))
    {
        Name = name;
        AddTargets(new BaseTarget());
        SourceRootPath = $@"[project.SharpmakeCsPath]/../src/{name}";
    }

    [Configure]
    public virtual void ConfigureAll(Configuration conf, Target target)
    {
        conf.Output = Configuration.OutputType.Lib;
        conf.ProjectPath = @"[project.SharpmakeCsPath]/../projects";

        conf.TargetPath = $@"[project.SharpmakeCsPath]/../bin/[target.Optimization]_[target.Platform]";
        conf.IntermediatePath = $@"[project.SharpmakeCsPath]/../tmp/intermediate/{Name}/obj_[target.Optimization]_[target.Platform]";

        conf.Options.Add(Options.Vc.Compiler.CppLanguageStandard.CPP20);
        conf.Options.Add(Options.Vc.Compiler.Exceptions.Enable);
        conf.Options.Add(Options.Vc.General.WarningLevel.Level4);
        conf.Options.Add(Options.Vc.Compiler.MultiProcessorCompilation.Enable);
        conf.Options.Add(new Options.Vc.Compiler.DisableSpecificWarnings("4100"));

        conf.Defines.Add("UNICODE", "_UNICODE");
        conf.Defines.Add("_WIN32_WINNT=0x0A00");

        // Global "src" include path, so cross-project includes look like "Renderer/Renderer.h"
        conf.IncludePaths.Add(@"[project.SharpmakeCsPath]/../src");

        if (target.Optimization == Optimization.Debug)
        {
            conf.Defines.Add("OMD_DEBUG");
            conf.Options.Add(Options.Vc.Compiler.RuntimeLibrary.MultiThreadedDebugDLL);
        }
        else
        {
            conf.Defines.Add("OMD_RELEASE");
            conf.Options.Add(Options.Vc.Compiler.RuntimeLibrary.MultiThreadedDLL);
        }

        if (target.Platform == Platform.win64)
        {
            conf.Defines.Add("OMD_WINDOWS");
        }
    }
}

[Sharpmake.Generate]
public class Foundation : OmdLabProjectBase
{
    public Foundation() : base("Foundation") { }
}

[Sharpmake.Generate]
public class Renderer : OmdLabProjectBase
{
    public Renderer() : base("Renderer") { }

    public override void ConfigureAll(Configuration conf, Target target)
    {
        base.ConfigureAll(conf, target);
        conf.AddPublicDependency<Foundation>(target);
    }
}

[Sharpmake.Generate]
public class Asset : OmdLabProjectBase
{
    public Asset() : base("Asset") { }

    public override void ConfigureAll(Configuration conf, Target target)
    {
        base.ConfigureAll(conf, target);
        conf.AddPublicDependency<Foundation>(target);
    }
}

[Sharpmake.Generate]
public class Engine : OmdLabProjectBase
{
    public Engine() : base("Engine") { }

    public override void ConfigureAll(Configuration conf, Target target)
    {
        base.ConfigureAll(conf, target);
        conf.AddPublicDependency<Foundation>(target);
        conf.AddPublicDependency<Renderer>(target);
        conf.AddPublicDependency<Asset>(target);
    }
}

[Sharpmake.Generate]
public class Game : OmdLabProjectBase
{
    public Game() : base("Game") { }

    public override void ConfigureAll(Configuration conf, Target target)
    {
        base.ConfigureAll(conf, target);
        conf.Output = Configuration.OutputType.Exe;
        conf.TargetPath = $@"[project.SharpmakeCsPath]/../bin/[target.Optimization]_[target.Platform]";

        conf.VcxprojUserFile = new Configuration.VcxprojUserFileSettings();
        conf.VcxprojUserFile.LocalDebuggerWorkingDirectory = @"[project.SharpmakeCsPath]/..";

        conf.AddPublicDependency<Engine>(target);
        conf.Options.Add(Options.Vc.Linker.SubSystem.Console);
    }
}

[Sharpmake.Generate]
public class OmdLabSolution : Solution
{
    public OmdLabSolution()
        : base(typeof(BaseTarget))
    {
        Name = "OmdLab";
        AddTargets(new BaseTarget());
    }

    [Configure]
    public void ConfigureAll(Configuration conf, Target target)
    {
        conf.SolutionPath = @"[solution.SharpmakeCsPath]/../projects";
        conf.SolutionFileName = $"OmdLab_{target.Platform}_{target.DevEnv}";

        conf.AddProject<Foundation>(target);
        conf.AddProject<Renderer>(target);
        conf.AddProject<Asset>(target);
        conf.AddProject<Engine>(target);
        conf.AddProject<Game>(target);
    }
}

public static class MainEntry
{
    [Sharpmake.Main]
    public static void SharpmakeMain(Arguments args)
    {
        args.Generate<OmdLabSolution>();
    }
}
