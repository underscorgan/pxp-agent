test_name 'Service Start stop/start, with configuration)'
@agent1 = agents[0]
@pxp_conf_file = '/etc/puppetlabs/pxp-agent/pxp-agent.conf'
if @agent1.platform.start_with?('windows')
  @pxp_conf_file = '/cygdrive/c/ProgramData/PuppetLabs/pxp-agent/etc/pxp-agent.conf'
end
@pxp_temp_file = '~/pxp-agent.conf'

# On teardown, restore configuration file
teardown do
  if @agent1.file_exist?(@pxp_temp_file)
    on(@agent1, "mv #{@pxp_temp_file} #{@pxp_conf_file}")
  end
end

def stop_service
  on(@agent1, puppet('resource service pxp-agent ensure=stopped'))
end

def start_service
  on(@agent1, puppet('resource service pxp-agent ensure=running'))
end

def assert_stopped
  on(@agent1, puppet('resource service pxp-agent ')) do |result|
    assert_match(/ensure => .stopped.,/, result.stdout,
                 "pxp-agent not in expected stopped state")
  end
end

def assert_running
  on(@agent1, puppet('resource service pxp-agent ')) do |result|
    assert_match(/ensure => .running.,/, result.stdout,
                 "pxp-agent not in expected running state")
  end
end

step 'C93070 - Service Start (from stopped, with configuration)'
stop_service
assert_stopped
start_service
assert_running

step 'C93069 - Service Stop (from running, with configuration)'
stop_service
assert_stopped

step 'Remove configuration'
stop_service
on(@agent1, "mv #{@pxp_conf_file} #{@pxp_temp_file}")

step 'C94686 - Service Start (from stopped, un-configured)'
start_service
assert_running

step 'C94687 - Service Stop (from running, un-configured)'
stop_service
assert_stopped

step 'Restore configuration'
on(@agent1, "mv #{@pxp_temp_file} #{@pxp_conf_file}")
